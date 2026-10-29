#ifndef AVIS_RHI_D3D12_SLOT_MAP_H
#define AVIS_RHI_D3D12_SLOT_MAP_H

// Generational slot map behind the public handles (docs/design/09-rhi.md §21.2, §3.2, D-75), and the private
// definition of detail::handle_access

#include "avis/error.h"
#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/leaf_mutex.h"
#include "avis/rhi/error.h"
#include "avis/rhi/types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace avis::rhi::d3d12
{
    // What a public handle<Tag> stores
    struct handle_bits
    {
        std::uint32_t index = 0;
        std::uint32_t generation = 0;

        friend constexpr bool operator==(handle_bits, handle_bits) noexcept = default;
    };
} // namespace avis::rhi::d3d12

namespace avis::rhi::detail
{
    // Reads and builds public handles (declared in rhi/types.h)
    struct handle_access
    {
        template<class Tag>
        static constexpr d3d12::handle_bits bits(const handle<Tag> value) noexcept
        {
            return { .index = value.index_, .generation = value.generation_ };
        }

        template<class Tag>
        static constexpr handle<Tag> make(const d3d12::handle_bits bits) noexcept
        {
            handle<Tag> value;
            value.index_ = bits.index;
            value.generation_ = bits.generation;
            return value;
        }
    };
} // namespace avis::rhi::detail

namespace avis::rhi::d3d12
{
    template<class Handle>
    struct handle_tag;

    template<class Tag>
    struct handle_tag<handle<Tag>>
    {
        using type = Tag;
    };

    template<class Tag>
    constexpr handle_bits to_bits(const handle<Tag> value) noexcept
    {
        return rhi::detail::handle_access::bits(value);
    }

    template<class Handle>
    constexpr Handle to_handle(const handle_bits bits) noexcept
    {
        return rhi::detail::handle_access::make<typename handle_tag<Handle>::type>(bits);
    }

    // A small tag per slot map, so a handle from another RHI (or another kind's map) is rejected (§3.2)
    inline std::uint32_t next_slot_map_tag() noexcept
    {
        static std::atomic<std::uint32_t> counter{ 0 };
        constexpr std::uint32_t tag_count = 1u << 12;
        // 1 … 4095: never 0, so bits with no tag never match
        return counter.fetch_add(1, std::memory_order_relaxed) % (tag_count - 1) + 1;
    }

    // Records live in fixed-size chunks that are never moved or freed before the map, so a pointer from find() stays
    // valid while its slot is alive. find() is lock-free; insert() and extract() take allocation_mutex_.
    //
    // Index bits: [31:20] the map's instance tag, [19:10] chunk, [9:0] slot in the chunk. Generations are odd while
    // the slot is alive and even while it's free, so generation 0 (a default handle) is never alive.
    template<class Record>
    class slot_map
    {
    public:
        static constexpr std::uint32_t chunk_size = 1024;
        static constexpr std::uint32_t chunk_count = 1024; // up to 1M records

        slot_map() noexcept : tag_{ next_slot_map_tag() } {}

        // Destroys the records still alive (shutdown, after wait_idle)
        ~slot_map()
        {
            for (std::uint32_t slot_index = 0; slot_index < size_; ++slot_index)
            {
                slot& entry = slot_at(slot_index);
                if ((entry.generation.load(std::memory_order_relaxed) & 1) != 0)
                {
                    record_of(entry)->~Record();
                }
            }
        }

        slot_map(const slot_map&) = delete;
        slot_map& operator=(const slot_map&) = delete;

        // Reuses a free slot (generation + 1) or appends one. The record is constructed under the mutex, so it should
        // be cheap to move in: build the native object first (§19.4).
        template<class... Arguments>
        result<handle_bits> insert(Arguments&&... arguments)
        {
            const std::lock_guard lock{ allocation_mutex_ };
            std::uint32_t slot_index = 0;
            const bool reused = !free_slots_.empty();
            if (reused)
            {
                slot_index = free_slots_.back();
            }
            else
            {
                if (size_ == chunk_size * chunk_count)
                {
                    return std::unexpected(make_error(
                        error_kind::out_of_memory, "slot map: more than 1,048,576 live objects of one kind"));
                }
                slot_index = size_;
                const std::uint32_t chunk = slot_index / chunk_size;
                if (chunks_[chunk].load(std::memory_order_relaxed) == nullptr)
                {
                    try
                    {
                        owned_chunks_.push_back(std::make_unique<slot[]>(chunk_size));
                    }
                    catch (const std::bad_alloc&)
                    {
                        return std::unexpected(make_error(error_kind::out_of_memory, "slot map: chunk allocation"));
                    }
                    chunks_[chunk].store(owned_chunks_.back().get(), std::memory_order_release);
                }
            }

            slot& entry = slot_at(slot_index);
            ::new (static_cast<void*>(entry.storage)) Record(std::forward<Arguments>(arguments)...);
            const std::uint32_t generation = entry.generation.load(std::memory_order_relaxed) + 1; // odd: alive
            entry.generation.store(generation, std::memory_order_release);

            if (reused)
            {
                free_slots_.pop_back();
            }
            else
            {
                ++size_;
                slots_used_.store(size_, std::memory_order_relaxed);
            }
            return handle_bits{ .index = (tag_ << tag_shift) | slot_index, .generation = generation };
        }

        // Slots ever used (live and free): the map's high-water mark, for tests that check it stops growing
        std::uint32_t slots_used() const noexcept
        {
            return slots_used_.load(std::memory_order_relaxed);
        }

        // nullptr for stale, invalid, out-of-range or foreign handles; never reads size_
        Record* find(const handle_bits handle) noexcept
        {
            slot* const entry = locate(handle);
            return entry != nullptr ? record_of(*entry) : nullptr;
        }

        const Record* find(const handle_bits handle) const noexcept
        {
            return const_cast<slot_map*>(this)->find(handle);
        }

        // Moves the record out and frees the slot (generation + 1, back on the free list). The caller hands the
        // moved record to the release queue.
        std::optional<Record> extract(const handle_bits handle) noexcept
        {
            static_assert(std::is_nothrow_move_constructible_v<Record>);
            const std::lock_guard lock{ allocation_mutex_ };
            slot* const entry = locate(handle);
            if (entry == nullptr)
            {
                return std::nullopt;
            }
            Record* const record = record_of(*entry);
            std::optional<Record> extracted{ std::move(*record) };
            record->~Record();
            entry->generation.store(handle.generation + 1, std::memory_order_release); // even: free
            free_slots_.push_back(handle.index & slot_mask); // out of memory here terminates (§23.8)
            return extracted;
        }

        // Shutdown and leak reports only: visits every live record under the mutex
        template<class Visitor>
        void for_each(Visitor&& visitor)
        {
            const std::lock_guard lock{ allocation_mutex_ };
            for (std::uint32_t slot_index = 0; slot_index < size_; ++slot_index)
            {
                slot& entry = slot_at(slot_index);
                const std::uint32_t generation = entry.generation.load(std::memory_order_relaxed);
                if ((generation & 1) != 0)
                {
                    visitor(
                        handle_bits{ .index = (tag_ << tag_shift) | slot_index, .generation = generation },
                        *record_of(entry));
                }
            }
        }

        // Builds handle bits for this map (white-box tests of out-of-range and foreign handles)
        handle_bits make_bits(const std::uint32_t slot_index, const std::uint32_t generation) const noexcept
        {
            return { .index = (tag_ << tag_shift) | (slot_index & slot_mask), .generation = generation };
        }

    private:
        static constexpr std::uint32_t tag_shift = 20;
        static constexpr std::uint32_t slot_mask = (1u << tag_shift) - 1;

        struct slot
        {
            std::atomic<std::uint32_t> generation{ 0 };
            alignas(Record) std::byte storage[sizeof(Record)];
        };

        static Record* record_of(slot& entry) noexcept
        {
            return std::launder(reinterpret_cast<Record*>(entry.storage));
        }

        // Under the mutex only: the chunk is known to exist
        slot& slot_at(const std::uint32_t slot_index) noexcept
        {
            return chunks_[slot_index / chunk_size].load(std::memory_order_relaxed)[slot_index % chunk_size];
        }

        slot* locate(const handle_bits handle) noexcept
        {
            if (handle.generation == 0)
            {
                return nullptr; // a default handle: nothing to report
            }
            if (!AVIS_RHI_CHECK(
                    handle.index >> tag_shift == tag_, "a handle from another render_hardware_interface (or kind)"))
            {
                return nullptr;
            }
            const std::uint32_t slot_index = handle.index & slot_mask;
            const std::uint32_t chunk = slot_index / chunk_size; // < chunk_count, given slot_mask
            slot* const chunk_slots = chunks_[chunk].load(std::memory_order_acquire);
            if (chunk_slots == nullptr)
            {
                return nullptr;
            }
            slot& entry = chunk_slots[slot_index % chunk_size];
            const std::uint32_t generation = entry.generation.load(std::memory_order_acquire);
            return generation == handle.generation && (generation & 1) != 0 ? &entry : nullptr;
        }

        const std::uint32_t tag_;
        leaf_mutex allocation_mutex_{ "slot_map::allocation_mutex_" };
        // Fixed table: an entry is written once (release, under the mutex) and read lock-free (acquire)
        std::array<std::atomic<slot*>, chunk_count> chunks_{};
        std::vector<std::unique_ptr<slot[]>> owned_chunks_; // guarded by allocation_mutex_
        std::vector<std::uint32_t> free_slots_; // guarded by allocation_mutex_
        std::uint32_t size_ = 0; // slots ever used; guarded by allocation_mutex_
        std::atomic<std::uint32_t> slots_used_{ 0 }; // a copy of size_ readable without the mutex
    };
} // namespace avis::rhi::d3d12

#endif
