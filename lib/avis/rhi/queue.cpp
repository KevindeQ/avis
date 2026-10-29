#include "avis/rhi/queue.h"

#include "avis/log.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/d3d12/shared_objects.h"
#include "avis/rhi/shared.h"
#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/command_list_pool.h"
#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/timestamp_query_set_state.h"
#include "avis/rhi/error.h"
#include "avis/rhi/render_hardware_interface.h"

#include <vector>

namespace avis::rhi
{
    template<queue_type Queue>
    queue<Queue>::queue(render_hardware_interface& hardware_interface) noexcept : state_{ &hardware_interface.state() }
    {}

    template<queue_type Queue>
    result<synchronization_point> queue<Queue>::submit(
        const std::span<command_list<Queue>> lists, const std::span<const synchronization_point> waits)
    {
        d3d12::device_queue& native_queue = state_->queues()[Queue];
        const bool alone = native_queue.enter_submit();
        AVIS_RHI_CHECK(alone, "submit: two threads submit to the same queue at once; serialise submissions per queue");
        struct leave_on_exit
        {
            d3d12::device_queue& target;
            bool entered;
            ~leave_on_exit()
            {
                if (entered)
                {
                    target.leave_submit();
                }
            }
        } const leave{ native_queue, alone };

        // Every list must be open and owned
        std::vector<d3d12::command_list_record*> records;
        records.reserve(lists.size());
        for (command_list<Queue>& list : lists)
        {
            d3d12::command_list_record* const record = list.record_;
            if (!AVIS_RHI_CHECK(
                    record != nullptr && record->generation.load(std::memory_order_relaxed) == list.generation_ &&
                        record->state.load(std::memory_order_acquire) == d3d12::list_state::recording,
                    "submit: a command list was already submitted, discarded or moved from"))
            {
                return std::unexpected(make_error(error_kind::invalid_handle, "submit: stale command list"));
            }
            records.push_back(record);
        }

        // Resolve the timestamp runs each list wrote (§24.4), then close it
        std::vector<ID3D12CommandList*> native_lists;
        native_lists.reserve(records.size());
        for (d3d12::command_list_record* const record : records)
        {
            for (const d3d12::timestamp_range& run : record->timestamps)
            {
                record->list->ResolveQueryData(
                    run.set->heap.get(),
                    D3D12_QUERY_TYPE_TIMESTAMP,
                    run.first,
                    run.end - run.first,
                    run.set->resolved.resource.get(),
                    std::uint64_t{ run.first } * sizeof(std::uint64_t));
            }
            record->list->Close();
            native_lists.push_back(record->list.get());
        }

        // The lists never execute when this fails: their allocators are reusable at once (value 0 is complete)
        const auto give_back = [&]
        {
            for (d3d12::command_list_record* const record : records)
            {
                record->pool->submitted(*record, { .queue = Queue, .value = 0 });
            }
            for (command_list<Queue>& list : lists)
            {
                list.record_ = nullptr;
            }
        };

        result<std::uint64_t> value = std::unexpected(avis::error{});
        try
        {
            value = native_queue.execute(native_lists, waits);
        }
        catch (...)
        {
            give_back(); // device_lost
            throw;
        }
        if (!value)
        {
            give_back();
            return std::unexpected(std::move(value.error()));
        }

        const synchronization_point point{ .queue = Queue, .value = *value };
        for (d3d12::command_list_record* const record : records)
        {
            record->pool->submitted(*record, point);
        }
        for (command_list<Queue>& list : lists)
        {
            list.record_ = nullptr; // the list and its recorders are invalid now
        }
        return point;
    }

    template<queue_type Queue>
    void queue<Queue>::signal(const shared_fence& fence, const std::uint64_t value)
    {
        if (auto signalled = state_->queues()[Queue].signal(*fence.state_->fence.get(), value); !signalled)
        {
            log::get("rhi")->error("queue::signal on a shared fence: {}", signalled.error());
        }
    }

    template<queue_type Queue>
    void queue<Queue>::wait(const shared_fence& fence, const std::uint64_t value)
    {
        if (auto waited = state_->queues()[Queue].wait(*fence.state_->fence.get(), value); !waited)
        {
            log::get("rhi")->error("queue::wait on a shared fence: {}", waited.error());
        }
    }

    template<queue_type Queue>
    synchronization_point queue<Queue>::last_submitted() const noexcept
    {
        return { .queue = Queue, .value = state_->queues()[Queue].last_submitted() };
    }

    template<queue_type Queue>
    std::uint64_t queue<Queue>::timestamp_frequency() const noexcept
    {
        return state_->queues()[Queue].timestamp_frequency();
    }

    template class queue<queue_type::graphics>;
    template class queue<queue_type::compute>;
    template class queue<queue_type::copy>;
} // namespace avis::rhi
