#ifndef AVIS_VISUALIZER_RENDERING_PASSES_LIGHTING_PASSES_H
#define AVIS_VISUALIZER_RENDERING_PASSES_LIGHTING_PASSES_H

// The lighting passes of a view (docs/design/11-renderer.md §2.3 passes 3, 8, 9, §6, docs/tasks/11-renderer.md §11.11),
// and the placeholder tonemap until 11.16:
//   cluster_build      compute, async: the clusters' view-space bounds, into the view's history; only added when the
//                      projection or the size changed since the bounds were built
//   light_cull         compute, async: cluster_lights (offset, count, dropped per cluster) and light_indices
//   deferred_lighting  compute, 8 × 8: hdr_color (RGBA16F), pre-exposed, with the sun's shadow from shadow_map and
//                      GTAO's occlusion (upsampled when at half resolution); optionally cascade_index (R8_UINT), each
//                      pixel's shadow cascade, for debugging and tests
//   sky                graphics: hdr_color where depth == 0
//   tonemap            compute: ldr_color (RGBA8_UNORM) = sRGB(x / (1 + x)) of hdr_color, a placeholder for 11.16

#include "rendering/environment_baker.h"
#include "rendering/frame_data.h"
#include "rendering/passes/gbuffer_pass.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/types.h"

#include <Eigen/Core>

#include <cstdint>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    // The pre-exposure of a manual EV100 (11 §10.3): 1 / (1.2 · 2^EV100), the camera's saturation-based exposure
    float manual_exposure(float ev100) noexcept;

    // light_indices' elements: cluster_maximum_lights per cluster
    inline constexpr std::uint64_t cluster_light_index_count =
        std::uint64_t{ gpu::cluster_count } * gpu::cluster_maximum_lights;

    struct cluster_inputs
    {
        rhi::compute_pipeline_handle build;
        rhi::compute_pipeline_handle cull;
        rhi::buffer_handle bounds; // the view's history: cluster_count cluster_bounds
        bool rebuild = false; // add cluster_build first
        Eigen::Matrix4f projection_inverse; // unjittered; for cluster_build
        graphics::texture_extent size;
        const frame_buffers& frame; // the lights
        graphics::texture depth_mips;
        std::uint32_t depth_mip_count = 1;
        gpu::view_constants view_constants;
        // Keep the lights of the clusters in front of their tile's opaque depth, and of tiles of sky only: the frame
        // draws transparents, which can be there (11.14)
        bool keep_front = false;
    };

    struct light_clusters
    {
        graphics::buffer bounds; // the version light_cull read
        graphics::buffer lights; // cluster_count cluster_lights
        graphics::buffer indices; // cluster_light_index_count uints
    };

    light_clusters add_light_clusters(graphics::render_graph& graph, const cluster_inputs& inputs);

    struct deferred_lighting_inputs
    {
        rhi::compute_pipeline_handle pipeline;
        const gbuffer_targets& gbuffer;
        graphics::texture depth; // the prepass's
        graphics::texture linear_depth;
        const light_clusters& clusters;
        const frame_buffers& frame;
        std::uint32_t sun = gpu::no_light; // index in the frame's lights
        const environment_resources& environment;
        float exposure = 1.0f;
        float ambient_intensity = 1.0f;
        gpu::view_constants view_constants;
        graphics::texture shadow_map; // invalid: no shadows, everything is lit
        graphics::texture ambient_occlusion; // GTAO's (R8); invalid: no occlusion beyond the material's
        std::uint32_t ambient_occlusion_scale = 1; // pixels per AO texel along each axis (gtao_scale)
        gpu::shadow_constants shadow{}; // the cascades of shadow_map; its shadow_map index is set when recording
        bool cascade_index = false; // write the cascade_index output
    };

    struct deferred_lighting_outputs
    {
        graphics::texture hdr_color; // RGBA16F, view size: 0 where nothing was drawn
        graphics::texture cascade_index; // R8_UINT, view size, when asked for: no_cascade where none applies
    };

    deferred_lighting_outputs
        add_deferred_lighting(graphics::render_graph& graph, const deferred_lighting_inputs& inputs);

    struct sky_inputs
    {
        rhi::graphics_pipeline_handle pipeline;
        graphics::texture hdr_color;
        graphics::texture depth;
        gpu::sky_constants sky;
        float exposure = 1.0f;
        gpu::view_constants view_constants;
    };

    struct sky_outputs
    {
        graphics::texture hdr_color;
        graphics::texture depth; // the version the pass wrote (its PSO writes none)
    };

    // The pipeline state the sky pass needs: depth EQUAL against the far plane, no depth write, no culling, into
    // hdr_color
    rhi::graphics_pipeline_state sky_pipeline_state() noexcept;

    sky_outputs add_sky_pass(graphics::render_graph& graph, const sky_inputs& inputs);

    // ldr_color, RGBA8_UNORM
    graphics::texture add_tonemap_preview(
        graphics::render_graph& graph, rhi::compute_pipeline_handle pipeline, graphics::texture hdr_color);
} // namespace avis::visualizer::rendering

#endif
