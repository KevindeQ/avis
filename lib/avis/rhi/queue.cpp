#include "avis/rhi/queue.h"

#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/render_hardware_interface.h"

namespace avis::rhi
{
    template<queue_type Queue>
    queue<Queue>::queue(render_hardware_interface& hardware_interface) noexcept : state_{ &hardware_interface.state() }
    {}

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
