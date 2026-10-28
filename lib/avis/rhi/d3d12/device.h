#ifndef AVIS_RHI_D3D12_DEVICE_H
#define AVIS_RHI_D3D12_DEVICE_H

// The native device and its health (docs/design/09-rhi.md §20.3.1, §20.4)

#include "avis/error.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/debug.h"
#include "avis/rhi/render_hardware_interface.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

namespace avis::rhi::d3d12
{
    // Windows 11 24H2 (D-05): enhanced barriers from the inbox D3D12 runtime
    inline constexpr std::uint32_t minimum_windows_build = 26100;

    // Every other class takes a device& for native calls, capabilities, debug names and the sticky removal flag.
    // Not movable: the info-queue callback points at its debug state.
    class device
    {
    public:
        // Steps 2–8 of §20.3.2 (step 5 through an adapter_selector); on failure nothing is left behind
        static result<std::unique_ptr<device>>
            create(const adapter_configuration& adapter, const debug_configuration& debug);

        // Detaches the info-queue callback; reports live objects in debug builds
        ~device();

        device(const device&) = delete;
        device& operator=(const device&) = delete;

        device_interface& native() noexcept;
        factory_interface& factory() noexcept;
        adapter_interface& adapter() noexcept;
        const hardware_capabilities& capabilities() const noexcept;
        const debug_state& debug() const noexcept;

        // Device removal (§20.4)
        bool lost() const noexcept;

        // Sets lost, logs the DRED report once (whichever thread sees the removal first) and throws rhi::device_lost
        [[noreturn]] void report_removed(std::string_view operation);

    private:
        device() = default;

        debug_state debug_;
        com_ptr<factory_interface> factory_;
        com_ptr<adapter_interface> adapter_;
        com_ptr<device_interface> device_;
        hardware_capabilities capabilities_;
        std::atomic<bool> lost_{ false };
        std::once_flag removal_report_;
    };

    // The running Windows build, from RtlGetVersion (GetVersionEx lies without a manifest); 0 if unknown
    std::uint32_t windows_build() noexcept;
} // namespace avis::rhi::d3d12

#endif
