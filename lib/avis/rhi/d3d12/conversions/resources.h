#ifndef AVIS_RHI_D3D12_CONVERSIONS_RESOURCES_H
#define AVIS_RHI_D3D12_CONVERSIONS_RESOURCES_H

// Resource descriptions → D3D12_RESOURCE_DESC1 and heap types (docs/design/09-rhi.md §20.2.3)

#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/resources.h"

#include <cstdint>

namespace avis::rhi::d3d12
{
    // Resource flags from the usages; depth textures that are also sampled are typeless. Never sets
    // ALLOW_SIMULTANEOUS_ACCESS: enhanced barriers don't need it.
    D3D12_RESOURCE_DESC1 to_native(const texture_description& description) noexcept;
    D3D12_RESOURCE_DESC1 to_native(const buffer_description& description) noexcept;
    D3D12_HEAP_TYPE to_native(memory_type value) noexcept;

    // Mip levels of a texture, with mip_levels = 0 resolved to the full chain
    std::uint32_t mip_level_count(const texture_description& description) noexcept;

    // Native array size: layers, 6 × cubes, or 1 for 3D textures
    std::uint32_t array_size(const texture_description& description) noexcept;
} // namespace avis::rhi::d3d12

#endif
