#ifndef AVIS_RHI_QUEUE_H
#define AVIS_RHI_QUEUE_H

// Queues and synchronization points (docs/design/09-rhi.md §5). Free of D3D12 and <windows.h>.

#include "avis/error.h"
#include "avis/rhi/types.h"

#include <cassert>
#include <cstdint>
#include <span>

namespace avis::rhi
{
    class render_hardware_interface;
    class shared_fence;
    template<queue_type>
    class command_list;

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

        // Executes the lists in order after GPU-side waits for `waits`. The lists are closed and become invalid, and so do
        // the recorders they handed out; their timestamp runs are resolved first (§24.4). Not thread-safe against itself:
        // callers serialise submissions per queue (asserted in debug builds). Throws device_lost after device removal.
        result<synchronization_point>
            submit(std::span<command_list<Queue>> lists, std::span<const synchronization_point> waits = {});

        // Cross-device fences (§13). wait() is a GPU-side wait that can't be cancelled: only use it when the signal is
        // already guaranteed to be submitted. Both throw device_lost after device removal; other failures are logged.
        void signal(const shared_fence& fence, std::uint64_t value);
        void wait(const shared_fence& fence, std::uint64_t value);

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
