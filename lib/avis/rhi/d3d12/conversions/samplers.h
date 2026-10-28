#ifndef AVIS_RHI_D3D12_CONVERSIONS_SAMPLERS_H
#define AVIS_RHI_D3D12_CONVERSIONS_SAMPLERS_H

// Sampler state → D3D12 (docs/design/09-rhi.md §20.2.3)

#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

namespace avis::rhi::d3d12
{
    D3D12_COMPARISON_FUNC to_native(comparison value) noexcept;
    D3D12_TEXTURE_ADDRESS_MODE to_native(address_mode value) noexcept;

    // Anisotropic in any stage makes the whole filter anisotropic; compare selects the comparison reduction
    D3D12_FILTER to_native_filter(filter minification, filter magnification, filter mip, bool compare) noexcept;
} // namespace avis::rhi::d3d12

#endif
