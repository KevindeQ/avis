#ifndef AVIS_RHI_D3D12_NATIVE_ACCESS_H
#define AVIS_RHI_D3D12_NATIVE_ACCESS_H

// Native objects behind the public API, for RHI internals and RHI tests only; never for applications
// (docs/design/09-rhi.md §20.2.4). native_list joins with the command lists. Code that needs the device calls
// rhi.state().device().native().

#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/types.h"

namespace avis::rhi::d3d12
{
    inline ID3D12CommandQueue&
        native_queue(render_hardware_interface& hardware_interface, const queue_type queue) noexcept
    {
        return hardware_interface.state().queues()[queue].native();
    }

    // nullptr for a stale handle
    inline ID3D12Resource*
        native_resource(render_hardware_interface& hardware_interface, const texture_handle texture) noexcept
    {
        const texture_record* const record = hardware_interface.state().resources().find(texture);
        return record != nullptr ? record->resource.get() : nullptr;
    }

    inline ID3D12Resource*
        native_resource(render_hardware_interface& hardware_interface, const buffer_handle buffer) noexcept
    {
        const buffer_record* const record = hardware_interface.state().resources().find(buffer);
        return record != nullptr ? record->resource.get() : nullptr;
    }
} // namespace avis::rhi::d3d12

#endif
