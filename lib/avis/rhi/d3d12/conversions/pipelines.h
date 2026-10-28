#ifndef AVIS_RHI_D3D12_CONVERSIONS_PIPELINES_H
#define AVIS_RHI_D3D12_CONVERSIONS_PIPELINES_H

// Pipeline and root signature enums → D3D12 (docs/design/09-rhi.md §20.2.3)

#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/root_signature.h"

namespace avis::rhi::d3d12
{
    D3D_PRIMITIVE_TOPOLOGY to_native(primitive_topology value) noexcept;
    D3D12_SHADER_VISIBILITY to_native(shader_visibility value) noexcept;

    // The pipeline state's topology class: triangle, line or point
    D3D12_PRIMITIVE_TOPOLOGY_TYPE to_native_topology_type(primitive_topology value) noexcept;
} // namespace avis::rhi::d3d12

#endif
