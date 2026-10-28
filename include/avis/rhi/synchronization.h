#ifndef AVIS_RHI_SYNCHRONIZATION_H
#define AVIS_RHI_SYNCHRONIZATION_H

// Waiting for GPU work (docs/design/09-rhi.md §4.3, §5.1). Never blocks. Free of D3D12 and <windows.h>.

#include "avis/rhi/detail/gpu_wait_sender.h"
#include "avis/rhi/queue.h"

#include <span>

namespace avis::rhi
{
    class render_hardware_interface;

    namespace d3d12
    {
        class device_state;
    } // namespace d3d12

    // A view (§4.3): non-owning, copyable, built from the RHI and never outliving it
    class synchronization
    {
    public:
        explicit synchronization(render_hardware_interface& hardware_interface) noexcept;

        // Any thread; value 0 is always complete
        bool is_complete(synchronization_point point) const;

        // Completes inline in start() when every point is already reached, otherwise from a later
        // completion_driver::poll_completions() on the thread that calls it (§5.1, §5.2)
        gpu_wait_sender when_complete(synchronization_point point);
        gpu_wait_sender when_complete(std::span<const synchronization_point> points);

    private:
        d3d12::device_state* state_;
    };
} // namespace avis::rhi

#endif
