#ifndef AVIS_RHI_QUEUE_H
#define AVIS_RHI_QUEUE_H

// Queues and synchronization points (docs/design/09-rhi.md §5). The queue<Queue> view follows with the device.

#include "avis/rhi/types.h"

#include <cassert>
#include <cstdint>

namespace avis::rhi
{
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
} // namespace avis::rhi

#endif
