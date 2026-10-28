#ifndef AVIS_RHI_D3D12_CONVERSIONS_BARRIERS_H
#define AVIS_RHI_D3D12_CONVERSIONS_BARRIERS_H

// Barrier types → D3D12 enhanced barrier types (docs/design/09-rhi.md §20.2.3). pipeline_synchronization and
// resource_access equal D3D12's values bit for bit, so those are casts; texture_layout goes through a table.

#include "avis/enum_flags.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

namespace avis::rhi::d3d12
{
    D3D12_BARRIER_SYNC to_native(avis::flags<pipeline_synchronization> value) noexcept;
    D3D12_BARRIER_ACCESS to_native(avis::flags<resource_access> value) noexcept;
    D3D12_BARRIER_LAYOUT to_native(texture_layout value) noexcept;

    // subresource_range::all resolves against the texture's mip and array counts (cube layers count faces)
    D3D12_BARRIER_SUBRESOURCE_RANGE to_native(subresource_range range, const texture_description& texture) noexcept;
} // namespace avis::rhi::d3d12

#endif
