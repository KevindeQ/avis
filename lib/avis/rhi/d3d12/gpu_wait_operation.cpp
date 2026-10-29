// Core of gpu_wait_sender (docs/design/09-rhi.md §23.5): the members of detail::gpu_wait_operation

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/completion_tracker.h"
#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/detail/gpu_wait_sender.h"

namespace avis::rhi::detail
{
    namespace
    {
        constexpr const char* lost_message = "GPU wait: the D3D12 device was removed";
    } // namespace

    gpu_wait_operation::gpu_wait_operation(d3d12::device_state& state, const gpu_wait_target& target) noexcept :
        state_{ &state }
    {
        queue_values = target.queue_values;
        shared_fence = target.shared_fence;
        shared_value = target.shared_value;
    }

    gpu_wait_operation::start_outcome gpu_wait_operation::start_wait(avis::error& error) noexcept
    {
        d3d12::device& device = state_->device();
        if (device.lost())
        {
            error = d3d12::device_lost_error(lost_message);
            return start_outcome::failed;
        }

        if (shared_fence == nullptr)
        {
            // Fast path without the tracker's lock: drop what is already reached
            d3d12::device_queues& queues = state_->queues();
            bool pending = false;
            for (std::size_t index = 0; index < queue_values.size(); ++index)
            {
                std::uint64_t& value = queue_values[index];
                if (value != 0 && queues[static_cast<queue_type>(index)].is_complete(value))
                {
                    value = 0;
                }
                pending = pending || value != 0;
            }
            if (device.lost())
            {
                error = d3d12::device_lost_error(lost_message);
                return start_outcome::failed;
            }
            if (!pending)
            {
                return start_outcome::reached;
            }
        }

        // A shared fence is checked by the tracker, under its lock (§23.9)
        switch (state_->completions().add(*this))
        {
        case d3d12::completion_tracker::add_outcome::added:
            return start_outcome::added;
        case d3d12::completion_tracker::add_outcome::reached:
            return start_outcome::reached;
        case d3d12::completion_tracker::add_outcome::stopped:
            return start_outcome::stopped;
        case d3d12::completion_tracker::add_outcome::failed:
            break;
        }
        error = d3d12::device_lost_error(lost_message);
        return start_outcome::failed;
    }

    bool gpu_wait_operation::cancel_wait() noexcept
    {
        return state_->completions().remove(*this);
    }
} // namespace avis::rhi::detail
