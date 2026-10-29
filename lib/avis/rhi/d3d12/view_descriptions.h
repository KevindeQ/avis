#ifndef AVIS_RHI_D3D12_VIEW_DESCRIPTIONS_H
#define AVIS_RHI_D3D12_VIEW_DESCRIPTIONS_H

// SRV and UAV descriptions of textures and buffers (docs/design/09-rhi.md §8.3): the persistent views of the resource
// manager and the transient views of descriptor ranges (§6) use the same ones.

#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <cstdint>

namespace avis::rhi::d3d12
{
    // Over `range` (all subresources by default). A sampled depth texture is read through its depth view format; a cube
    // over all its faces is a cube (array) view, a partial range of faces a 2D array view.
    D3D12_SHADER_RESOURCE_VIEW_DESC
        texture_srv(const texture_description& description, subresource_range range = all_subresources) noexcept;

    // One mip, every layer
    D3D12_UNORDERED_ACCESS_VIEW_DESC texture_uav(const texture_description& description, std::uint32_t mip) noexcept;

    // Structured when structure_stride > 0, raw (R32_TYPELESS) otherwise
    D3D12_SHADER_RESOURCE_VIEW_DESC buffer_srv(const buffer_description& description) noexcept;
    D3D12_UNORDERED_ACCESS_VIEW_DESC buffer_uav(const buffer_description& description) noexcept;
} // namespace avis::rhi::d3d12

#endif
