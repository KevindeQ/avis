#ifndef AVIS_RHI_D3D12_CAPABILITY_GATE_H
#define AVIS_RHI_D3D12_CAPABILITY_GATE_H

// The capability gate (docs/design/09-rhi.md §4.4, §20.3.2)

#include "avis/error.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/render_hardware_interface.h"

namespace avis::rhi::d3d12
{
    // One check per row of §4.4; the first failure is unsupported_hardware naming the check, the required and the
    // reported value. On success fills the optional fields of `capabilities` (copy-queue timestamps, wave lane
    // counts, timestamp frequency). The device_interface row is checked by the adapter selector's QueryInterface.
    result<void> check_capabilities(device_interface& device, hardware_capabilities& capabilities);
} // namespace avis::rhi::d3d12

#endif
