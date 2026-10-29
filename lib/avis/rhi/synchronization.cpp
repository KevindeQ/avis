#include "avis/rhi/synchronization.h"

#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/render_hardware_interface.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace avis::rhi
{
    synchronization::synchronization(render_hardware_interface& hardware_interface) noexcept :
        state_{ &hardware_interface.state() }
    {}

    bool synchronization::is_complete(const synchronization_point point) const
    {
        return state_->queues().is_complete(point);
    }

    gpu_wait_sender synchronization::when_complete(const synchronization_point point)
    {
        return when_complete(std::span{ &point, 1 });
    }

    gpu_wait_sender synchronization::when_complete(const std::span<const synchronization_point> points)
    {
        // Any set of points reduces to the latest value per queue (§23.4)
        std::array<std::uint64_t, 3> values{};
        for (const synchronization_point point : points)
        {
            std::uint64_t& value = values[static_cast<std::size_t>(point.queue)];
            if (point.value > value)
            {
                value = point.value;
            }
        }
        return gpu_wait_sender{ *state_, detail::gpu_wait_target{ .queue_values = values } };
    }
} // namespace avis::rhi
