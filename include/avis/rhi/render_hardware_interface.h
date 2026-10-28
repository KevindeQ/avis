#ifndef AVIS_RHI_RENDER_HARDWARE_INTERFACE_H
#define AVIS_RHI_RENDER_HARDWARE_INTERFACE_H

// Root object of the RHI (docs/design/09-rhi.md §4.2, D-18). Everything beyond creation, device information and
// wait_idle goes through the subsystem views (§4.3). Free of D3D12 and <windows.h>.

#include "avis/error.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace avis::rhi
{
    namespace d3d12
    {
        class device_state; // lib/avis/rhi/d3d12/device_state.h
    } // namespace d3d12

    struct hardware_capabilities
    {
        std::string adapter_name; // UTF-8, from DXGI_ADAPTER_DESC3
        adapter_luid luid;
        std::uint32_t vendor_id = 0;
        std::uint32_t device_id = 0;
        std::uint64_t dedicated_video_memory = 0;
        bool software_adapter = false; // WARP
        bool copy_queue_timestamps = false; // D3D12_OPTIONS3::CopyQueueTimestampQueriesSupported
        std::uint32_t minimum_wave_lane_count = 0; // D3D12_OPTIONS1
        std::uint32_t maximum_wave_lane_count = 0;
        std::uint64_t timestamp_frequency = 0; // graphics queue ticks per second; per queue: queue::timestamp_frequency
    };

    // Local (dedicated) video memory, from IDXGIAdapter3::QueryVideoMemoryInfo
    struct memory_budget
    {
        std::uint64_t budget = 0; // what the OS currently grants this process
        std::uint64_t usage = 0;
    };

    // Not copyable or movable: handles, views, command lists and senders refer back to it
    class render_hardware_interface
    {
    public:
        // Creates the device after the capability gate (§4.4), then the subsystems. No root signature: the caller
        // creates the ones it needs (§12.1). Starts no thread: GPU waits complete when the caller drives
        // completion_driver::poll_completions() (§5.2). Fails with invalid_argument for configuration values out of
        // range (§4.1), before any device is created.
        static result<std::unique_ptr<render_hardware_interface>>
            create(const render_hardware_interface_configuration& configuration);

        // Waits for the GPU and releases everything, including deferred releases. No command list may be open and
        // no sender of this object may still be running. Must not run on a parallel worker or inside a completion.
        ~render_hardware_interface();

        render_hardware_interface(const render_hardware_interface&) = delete;
        render_hardware_interface& operator=(const render_hardware_interface&) = delete;

        const hardware_capabilities& capabilities() const noexcept;

        // From the memory allocator (§21.4); a few µs, so the renderer reads it once per frame at most
        memory_budget local_memory_budget() const;

        // Blocks until every queue is idle, then runs completion_driver::poll_completions(). The only blocking call
        // besides the destructor: shutdown and tests (D-93).
        void wait_idle();

        // The private state behind the RHI: the views' one way in (§4.3), and native_access.h's. For RHI code only;
        // device_state is an incomplete type outside lib/avis/rhi/.
        d3d12::device_state& state() noexcept;

    private:
        explicit render_hardware_interface(std::unique_ptr<d3d12::device_state> state) noexcept;

        std::unique_ptr<d3d12::device_state> state_;
    };
} // namespace avis::rhi

#endif
