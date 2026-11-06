#ifndef AVIS_VISUALIZER_RENDERING_PASSES_DEPTH_PASSES_H
#define AVIS_VISUALIZER_RENDERING_PASSES_DEPTH_PASSES_H

// The depth prepass and linearize depth (docs/design/11-renderer.md §2.3 passes 1 and 2, docs/tasks/11-renderer.md
// §11.10).
//
// depth_prepass: a D32F reverse-Z depth buffer named "depth", cleared to 0 (the far plane), written by the range
// scene/camera/world/opaque with each pipeline set's depth PSO. Opaque triangles and lines are depth-only; mask
// materials and points have a pixel shader that discards (depth_prepass.hlsl).
//
// linearize_depth: a compute pass (graphics queue) writing "linear_depth" (R32F, metres in front of the camera,
// +infinity where nothing was drawn) and mip 0 of "depth_mips" (RG32F, min in R and max in G), then one compute pass
// per further mip ("depth_mips_<mip>") reducing 2 × 2 texels (3 on the last row or column of an odd size).

#include "rendering/passes/scene_range_pass.h"

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/pipeline.h"

#include <cstdint>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    // depth_mips' mip count for a view of at least 16 × 16 pixels; smaller views stop at 1 × 1
    inline constexpr std::uint32_t depth_mip_count = 5;

    // The mip count of depth_mips for a `width` × `height` view
    std::uint32_t depth_mip_levels(std::uint32_t width, std::uint32_t height) noexcept;

    // The texel range of mip `mip - 1` that texel `texel` of mip `mip` reduces, along one axis of `source_extent`
    // texels: [first, last], both included. Exposed for the tests' CPU reduction.
    struct reduction_span
    {
        std::uint32_t first = 0;
        std::uint32_t last = 0;
    };

    reduction_span depth_mip_span(std::uint32_t texel, std::uint32_t source_extent) noexcept;

    // The camera's depth buffer after the prepass
    graphics::texture add_depth_prepass(graphics::render_graph& graph, const scene_pass_inputs& inputs);

    struct linearized_depth
    {
        graphics::texture linear_depth;
        graphics::texture depth_mips; // every mip written
    };

    struct linearize_pipelines
    {
        rhi::compute_pipeline_handle linearize; // linearize_depth.hlsl
        rhi::compute_pipeline_handle reduce; // depth_mips.hlsl
    };

    linearized_depth add_linearize_depth(
        graphics::render_graph& graph,
        graphics::texture depth,
        const linearize_pipelines& pipelines,
        const gpu::view_constants& view_constants,
        graphics::texture_extent size);
} // namespace avis::visualizer::rendering

#endif
