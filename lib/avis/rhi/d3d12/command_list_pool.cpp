#include "avis/rhi/d3d12/command_list_pool.h"

#include "avis/log.h"
#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/completion_source.h"
#include "avis/rhi/d3d12/conversions/queues.h"
#include "avis/rhi/d3d12/descriptor_heaps.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/error.h"

#include <algorithm>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        constexpr std::uint32_t no_slot = std::numeric_limits<std::uint32_t>::max();

        // Gives the thread's number back when the thread exits
        struct thread_slot_holder
        {
            std::uint32_t number = no_slot;

            ~thread_slot_holder()
            {
                if (number != no_slot)
                {
                    thread_slot_numbers::instance().give_back(number);
                }
            }
        };

        thread_local thread_slot_holder current_holder;

        constexpr std::size_t index(const queue_type queue) noexcept
        {
            return static_cast<std::size_t>(queue);
        }
    } // namespace

    // ---- thread slot numbers ----

    thread_slot_numbers& thread_slot_numbers::instance() noexcept
    {
        // Never destroyed: threads may give their number back during process exit
        static thread_slot_numbers* const numbers = new thread_slot_numbers;
        return *numbers;
    }

    std::uint32_t thread_slot_numbers::take() noexcept
    {
        const std::lock_guard lock{ mutex_ };
        live_.fetch_add(1, std::memory_order_relaxed);
        if (!free_.empty())
        {
            const std::uint32_t number = free_.front();
            free_.erase(free_.begin());
            return number;
        }
        return next_++;
    }

    void thread_slot_numbers::give_back(const std::uint32_t slot) noexcept
    {
        const std::lock_guard lock{ mutex_ };
        live_.fetch_sub(1, std::memory_order_relaxed);
        free_.insert(std::ranges::lower_bound(free_, slot), slot); // out of memory terminates
    }

    std::uint32_t thread_slot_numbers::live_count() const noexcept
    {
        return live_.load(std::memory_order_relaxed);
    }

    std::uint32_t current_thread_slot() noexcept
    {
        if (current_holder.number == no_slot)
        {
            current_holder.number = thread_slot_numbers::instance().take();
        }
        return current_holder.number;
    }

    // ---- command_list_pool ----

    command_list_pool::command_list_pool(
        device& device,
        const completion_source& completions,
        descriptor_heaps& descriptors,
        lifetime_manager& lifetime,
        const recording_context& recording) noexcept :
        device_{ device },
        completions_{ completions },
        descriptors_{ descriptors },
        lifetime_{ lifetime },
        recording_{ recording }
    {}

    command_list_pool::~command_list_pool()
    {
        // The GPU is idle (~device_state ran wait_idle) and every command_list is gone. A record still recording is a
        // leaked command_list.
        for (const auto& slot : slots_)
        {
            for (const auto& records : slot->records)
            {
                for (const auto& record : records)
                {
                    if (record->state.load(std::memory_order_acquire) == list_state::recording)
                    {
                        try
                        {
                            log::get("rhi")->warn("command_list_pool: a command list was still open at shutdown");
                        }
                        catch (...)
                        {
                        }
                    }
                }
            }
        }
    }

    result<std::unique_ptr<command_list_pool>> command_list_pool::create(
        device& device,
        const completion_source& completions,
        descriptor_heaps& descriptors,
        lifetime_manager& lifetime,
        const recording_context& recording)
    {
        return std::unique_ptr<command_list_pool>{ new command_list_pool{
            device, completions, descriptors, lifetime, recording } };
    }

    result<command_list_thread_slot*> command_list_pool::current_slot()
    {
        const std::uint32_t number = current_thread_slot();
        if (const slot_table* const table = slot_table_.load(std::memory_order_acquire);
            table != nullptr && number < table->size())
        {
            return (*table)[number];
        }

        const std::lock_guard lock{ mutex_ };
        const slot_table* const current = slot_table_.load(std::memory_order_relaxed);
        if (current != nullptr && number < current->size())
        {
            return (*current)[number];
        }
        try
        {
            // A new, larger table with a slot for every number; readers of the old one keep using it
            const std::size_t size = std::max<std::size_t>(
                { std::size_t{ number } + 1, current != nullptr ? current->size() * 2 : 4 });
            auto table = std::make_unique<slot_table>();
            table->reserve(size);
            if (current != nullptr)
            {
                table->assign(current->begin(), current->end());
            }
            while (table->size() < size)
            {
                slots_.push_back(std::make_unique<command_list_thread_slot>());
                table->push_back(slots_.back().get());
            }
            slot_table_.store(table.get(), std::memory_order_release);
            tables_.push_back(std::move(table));
        }
        catch (const std::bad_alloc&)
        {
            return std::unexpected(make_error(error_kind::out_of_memory, "command_list_pool: slot table growth"));
        }

        const std::uint32_t live = thread_slot_numbers::instance().live_count();
        const std::uint32_t limit = 2 * std::max(1u, std::thread::hardware_concurrency());
        if (live > limit && !slot_warning_logged_.exchange(true, std::memory_order_relaxed))
        {
            log::get("rhi")->warn(
                "command_list_pool: {} threads hold a recording slot, more than 2 × hardware_concurrency ({})",
                live,
                limit);
        }
        return slot_table_.load(std::memory_order_relaxed)->at(number);
    }

    result<allocator_entry*> command_list_pool::take_allocator(command_allocator_pool& pool, const queue_type queue)
    {
        // The hand-back stack: sort submitted entries into in_flight by point value, discarded ones into free
        for (allocator_entry* entry = pool.returned.exchange(nullptr, std::memory_order_acquire); entry != nullptr;)
        {
            allocator_entry* const next = entry->next_returned;
            entry->next_returned = nullptr;
            if (entry->last_use)
            {
                const auto position = std::ranges::upper_bound(
                    pool.in_flight, entry->last_use->value, {}, [](const allocator_entry* other) { return other->last_use->value; });
                pool.in_flight.insert(position, entry);
            }
            else
            {
                pool.free.push_back(entry);
            }
            entry = next;
        }

        allocator_entry* entry = nullptr;
        if (!pool.free.empty())
        {
            entry = pool.free.back();
            pool.free.pop_back();
        }
        else if (!pool.in_flight.empty() && completions_.is_complete(*pool.in_flight.front()->last_use))
        {
            entry = pool.in_flight.front();
            pool.in_flight.pop_front();
        }
        else
        {
            auto created = std::make_unique<allocator_entry>();
            if (auto result = check_device(
                    device_,
                    device_.native().CreateCommandAllocator(to_native(queue), IID_PPV_ARGS(&created->allocator)),
                    "CreateCommandAllocator");
                !result)
            {
                return std::unexpected(std::move(result.error()));
            }
            created->home = &pool;
            entry = created.get();
            pool.entries.push_back(std::move(created));
            allocators_created_.fetch_add(1, std::memory_order_relaxed);
        }

        // Trim the surplus; only the owning thread frees allocators, so nothing races with this
        while (pool.free.size() > maximum_free_allocators)
        {
            allocator_entry* const surplus = pool.free.back();
            pool.free.pop_back();
            std::erase_if(pool.entries, [&](const auto& owned) { return owned.get() == surplus; });
            allocators_released_.fetch_add(1, std::memory_order_relaxed);
        }

        entry->last_use.reset();
        if (auto reset = check_device(device_, entry->allocator->Reset(), "ID3D12CommandAllocator::Reset"); !reset)
        {
            pool.free.push_back(entry);
            return std::unexpected(std::move(reset.error()));
        }
        return entry;
    }

    result<command_list_record*>
        command_list_pool::take_record(command_list_thread_slot& slot, const queue_type queue, allocator_entry& entry)
    {
        auto& records = slot.records[index(queue)];
        for (const auto& record : records)
        {
            if (record->state.load(std::memory_order_acquire) == list_state::free)
            {
                if (auto reset = check_device(
                        device_, record->list->Reset(entry.allocator.get(), nullptr), "ID3D12GraphicsCommandList::Reset");
                    !reset)
                {
                    return std::unexpected(std::move(reset.error()));
                }
                return record.get();
            }
        }

        auto record = std::make_unique<command_list_record>();
        record->context = &recording_;
        record->pool = this;
        record->queue = queue;
        // Created closed and without an allocator, then reset onto the one taken
        if (auto created = check_device(
                device_,
                device_.native().CreateCommandList1(0, to_native(queue), D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&record->list)),
                "CreateCommandList1");
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        if (auto reset = check_device(
                device_, record->list->Reset(entry.allocator.get(), nullptr), "ID3D12GraphicsCommandList::Reset");
            !reset)
        {
            return std::unexpected(std::move(reset.error()));
        }
        records.push_back(std::move(record));
        return records.back().get();
    }

    result<command_list_record*> command_list_pool::acquire(const queue_type queue, const std::string_view debug_name)
    {
        if (device_.lost())
        {
            return std::unexpected(device_lost_error("acquire: the D3D12 device was removed"));
        }
        auto slot = current_slot();
        if (!slot)
        {
            return std::unexpected(std::move(slot.error()));
        }
        command_allocator_pool& pool = (*slot)->allocators[index(queue)];
        auto entry = take_allocator(pool, queue);
        if (!entry)
        {
            return std::unexpected(std::move(entry.error()));
        }
        auto record = take_record(**slot, queue, **entry);
        if (!record)
        {
            (*entry)->last_use.reset();
            pool.free.push_back(*entry);
            return std::unexpected(std::move(record.error()));
        }

        command_list_record& taken = **record;
        taken.allocator = *entry;
        taken.generation.fetch_add(1, std::memory_order_relaxed);
        taken.pipeline = bound_pipeline::none;
        taken.graphics_root = nullptr;
        taken.compute_root = nullptr;
        taken.timestamps.clear();
        if (queue != queue_type::copy) // copy lists can't bind descriptor heaps
        {
            const std::array heaps = descriptors_.shader_visible_heaps();
            taken.list->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
        }
        set_debug_name(device_.debug(), *taken.list.get(), debug_name);
        taken.epoch = lifetime_.list_opened();
        taken.state.store(list_state::recording, std::memory_order_relaxed); // the owning thread only
        return &taken;
    }

    void command_list_pool::hand_back(allocator_entry& entry) noexcept
    {
        std::atomic<allocator_entry*>& returned = entry.home->returned;
        allocator_entry* head = returned.load(std::memory_order_relaxed);
        do
        {
            entry.next_returned = head;
        } while (!returned.compare_exchange_weak(head, &entry, std::memory_order_release, std::memory_order_relaxed));
    }

    void command_list_pool::submitted(command_list_record& record, const synchronization_point point) noexcept
    {
        allocator_entry* const entry = std::exchange(record.allocator, nullptr);
        entry->last_use = point;
        hand_back(*entry);
        lifetime_.list_closed(record.epoch);
        record.timestamps.clear();
        record.state.store(list_state::free, std::memory_order_release); // last write
    }

    void command_list_pool::discard(command_list_record& record) noexcept
    {
        record.list->Close(); // the list never executes; a failure here changes nothing
        allocator_entry* const entry = std::exchange(record.allocator, nullptr);
        entry->last_use.reset(); // reusable at once
        hand_back(*entry);
        lifetime_.list_closed(record.epoch);
        record.timestamps.clear();
        try
        {
            log::get("rhi")->debug("command list discarded without submit");
        }
        catch (...)
        {
        }
        record.state.store(list_state::free, std::memory_order_release); // last write
    }

    std::size_t command_list_pool::allocators_created() const noexcept
    {
        return allocators_created_.load(std::memory_order_relaxed);
    }

    std::size_t command_list_pool::allocators_released() const noexcept
    {
        return allocators_released_.load(std::memory_order_relaxed);
    }
} // namespace avis::rhi::d3d12
