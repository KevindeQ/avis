#ifndef AVIS_RHI_D3D12_CONVERSIONS_FORMATS_H
#define AVIS_RHI_D3D12_CONVERSIONS_FORMATS_H

// format → DXGI_FORMAT (docs/design/09-rhi.md §20.2.3)

#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/formats.h"

namespace avis::rhi::d3d12
{
    DXGI_FORMAT to_native(format value) noexcept;

    // Depth textures that are also sampled are created typeless: D32_FLOAT → R32_TYPELESS. Other formats unchanged.
    DXGI_FORMAT to_native_typeless(format value) noexcept;

    // The SRV format of a depth texture: D32_FLOAT → R32_FLOAT. Other formats unchanged.
    DXGI_FORMAT to_native_depth_view(format value) noexcept;
} // namespace avis::rhi::d3d12

#endif
