#ifndef AVIS_RHI_D3D12_ADAPTER_SELECTOR_H
#define AVIS_RHI_D3D12_ADAPTER_SELECTOR_H

// Adapter selection, step 5 of the creation sequence (docs/design/09-rhi.md §4.5, §20.3.2)

#include "avis/error.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace avis::rhi::d3d12
{
    // One enumerated adapter, read from DXGI_ADAPTER_DESC3 before any device is created
    struct adapter_candidate
    {
        com_ptr<adapter_interface> adapter;
        std::string name; // UTF-8
        adapter_luid luid;
        std::uint32_t vendor_id = 0;
        std::uint32_t device_id = 0;
        std::uint64_t dedicated_video_memory = 0;
        bool software = false; // DXGI_ADAPTER_FLAG3_SOFTWARE (WARP)
    };

    struct adapter_rejection
    {
        adapter_candidate candidate;
        avis::error reason; // from D3D12CreateDevice, QueryInterface or check_capabilities
    };

    // The winner: the adapter, the device created on it, and its capabilities
    struct selected_adapter
    {
        com_ptr<adapter_interface> adapter;
        com_ptr<device_interface> device;
        hardware_capabilities capabilities; // descriptive fields from the candidate, the rest from the gate
    };

    // Short-lived: device::create makes one on the stack, calls select() once and moves the result into the device.
    // Not movable, since it holds a reference to the factory.
    class adapter_selector
    {
    public:
        adapter_selector(factory_interface& factory, const adapter_configuration& configuration);

        adapter_selector(const adapter_selector&) = delete;
        adapter_selector& operator=(const adapter_selector&) = delete;

        // The candidates in the order select() tries them (§4.5):
        //   LUID configured → that one adapter only (no fallback: Qt interop needs exactly that adapter);
        //   otherwise       → hardware adapters by EnumAdapterByGpuPreference(HIGH_PERFORMANCE),
        //                     then EnumWarpAdapter when allow_software_adapter is set.
        // No devices are created. Fails with adapter_not_found when the configured LUID doesn't exist.
        result<std::vector<adapter_candidate>> enumerate() const;

        // Tries each candidate: create the device, query device_interface, check_capabilities. Returns the first
        // that passes. On failure: adapter_not_found (no candidates) or unsupported_hardware, whose message lists
        // every candidate with its rejection reason. The info log names the chosen adapter.
        result<selected_adapter> select();

        // Every candidate rejected by the last select(), in the order tried
        std::span<const adapter_rejection> rejections() const noexcept;

    private:
        result<selected_adapter> try_candidate(const adapter_candidate& candidate) const;
        avis::error failure() const;

        factory_interface& factory_;
        adapter_configuration configuration_;
        std::vector<adapter_rejection> rejections_;
    };
} // namespace avis::rhi::d3d12

#endif
