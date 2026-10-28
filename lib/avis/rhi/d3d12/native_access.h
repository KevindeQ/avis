#ifndef AVIS_RHI_D3D12_NATIVE_ACCESS_H
#define AVIS_RHI_D3D12_NATIVE_ACCESS_H

// Native objects behind the public API, for RHI internals and RHI tests only; never for applications
// (docs/design/09-rhi.md §20.2.4). The resource and list accessors join with their subsystems. Code that needs the
// device calls rhi.state().device().native().

#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/types.h"

namespace avis::rhi::d3d12
{
    inline ID3D12CommandQueue&
        native_queue(render_hardware_interface& hardware_interface, const queue_type queue) noexcept
    {
        return hardware_interface.state().queues()[queue].native();
    }
} // namespace avis::rhi::d3d12

#endif
