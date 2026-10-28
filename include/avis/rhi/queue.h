#ifndef AVIS_RHI_QUEUE_H
#define AVIS_RHI_QUEUE_H

// Queues and synchronization points (docs/design/09-rhi.md §5). Free of D3D12 and <windows.h>.

#include "avis/rhi/types.h"

#include <cassert>
#include <cstdint>

namespace avis::rhi
{
    class render_hardware_interface;

    namespace d3d12
    {
        class device_state;
    } // namespace d3d12

    // A point on a queue's timeline fence. value 0 means "nothing to wait for" and is always complete.
    struct synchronization_point
    {
        queue_type queue = queue_type::graphics;
        std::uint64_t value = 0;

        friend constexpr bool operator==(synchronization_point, synchronization_point) noexcept = default;
    };

    // The later of two points on the same queue. A point with value 0 waits for nothing, so the other one wins
    // whatever its queue.
    constexpr synchronization_point latest(const synchronization_point a, const synchronization_point b) noexcept
    {
        if (a.value == 0)
        {
            return b;
        }
        if (b.value == 0)
        {
            return a;
        }
        assert(a.queue == b.queue && "latest: points on different queues");
        return a.value >= b.value ? a : b;
    }

    // A view of one of the RHI's three queues (§4.3): non-owning, copyable, built from the RHI and never outliving it.
    // Explicitly instantiated for the three queue types. submit follows with the command lists, signal and wait with
    // shared fences.
    template<queue_type Queue>
    class queue
    {
    public:
        explicit queue(render_hardware_interface& hardware_interface) noexcept;

        // The point of the latest submission; value 0 before the first
        synchronization_point last_submitted() const noexcept;

        // Ticks per second, for timestamp_query_set (§6); 0 when the queue has no timestamps
        std::uint64_t timestamp_frequency() const noexcept;

    private:
        d3d12::device_state* state_;
    };

    extern template class queue<queue_type::graphics>;
    extern template class queue<queue_type::compute>;
    extern template class queue<queue_type::copy>;
} // namespace avis::rhi

#endif
