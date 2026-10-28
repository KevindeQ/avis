#ifndef AVIS_RHI_COMPLETION_DRIVER_H
#define AVIS_RHI_COMPLETION_DRIVER_H

// Running the RHI's housekeeping (docs/design/09-rhi.md §4.3, §5.2, D-93). For whoever drives the RHI (the
// application's event loop, gpu_completion_thread, tests), not for components that only wait. Never blocks.
// Free of D3D12 and <windows.h>.

#include <cstddef>

namespace avis::rhi
{
    class render_hardware_interface;

    namespace d3d12
    {
        class device_state;
    } // namespace d3d12

    // A view (§4.3): non-owning, copyable, built from the RHI and never outliving it
    class completion_driver
    {
    public:
        explicit completion_driver(render_hardware_interface& hardware_interface) noexcept;

        // One non-blocking pass, like asio's io_context::poll(): completes the GPU waits that are due, then frees
        // destroyed objects whose GPU work is done (capped per call). Any thread, concurrently, also inside a
        // completion. Returns the waits it completed.
        std::size_t poll_completions() noexcept;

        // Auto-reset Win32 event (a HANDLE), signalled when poll_completions() has something to do (and occasionally
        // when it hasn't). The RHI owns it: wait on it, never close or reset it.
        void* completion_event() const noexcept;

        // GPU waits pending, or destroyed objects not yet freed
        bool has_pending_work() const noexcept;

    private:
        d3d12::device_state* state_;
    };
} // namespace avis::rhi

#endif
