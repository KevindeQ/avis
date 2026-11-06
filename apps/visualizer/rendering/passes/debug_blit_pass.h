#ifndef AVIS_VISUALIZER_RENDERING_PASSES_DEBUG_BLIT_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_DEBUG_BLIT_PASS_H

// The debug blit and the pixel inspector (docs/design/11-renderer.md §11, docs/tasks/11-renderer.md §11.20), after the
// overlays and picking, on a frame with debug selections:
//   debug_blit         graphics: one fullscreen triangle per blit into its rectangle of ldr_color, in order (later ones
//                      on top), with debug_blit.hlsl. Each source is read with read_for_debug, which keeps it and its
//                      producers alive and out of aliasing (10 §9).
//   debug_inspect      compute: the inspector's texel into a small buffer
//   debug_readback     copy: that buffer into the readback buffer the renderer imported
// And a view's cluster statistics, for scene_renderer::debug_information:
//   cluster_statistics compute: light_cull's cluster_lights summed into a small buffer (cluster_statistics.hlsl)
//   cluster_statistics_readback  copy: into the readback buffer the renderer imported

#include "rendering/debug_blit.h"
#include "rendering/view.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/pipeline.h"

#include <cstddef>
#include <optional>
#include <span>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    // The debug blit's PSO state: one RGBA8_UNORM target (ldr_color), no depth, no blending
    rhi::graphics_pipeline_state debug_blit_pipeline_state() noexcept;

    // One blit, resolved against the frame's resources
    struct debug_blit_source
    {
        graphics::texture texture; // the selected texture; invalid for cluster_lights
        graphics::buffer clusters; // cluster_lights, with gpu::debug_source_clusters
        std::uint32_t source_kind = gpu::debug_source_float; // gpu::debug_source_*
        bool is_array = false; // the texture has array slices
        graphics::texture_extent source_size{ 1, 1 }; // texels of the selected mip; the view's size for clusters
        debug_selection selection; // the mip, slice, mode, channels and range
        view_rectangle rectangle;
    };

    struct debug_inspection
    {
        std::size_t blit = 0; // the blit whose source is read
        texel_position texel;
        graphics::buffer readback; // imported; at least debug_inspector_bytes, written from offset 0
    };

    struct debug_blit_inputs
    {
        rhi::graphics_pipeline_handle blit; // debug_blit.hlsl vertex_main and pixel_main
        rhi::compute_pipeline_handle inspect; // debug_blit.hlsl inspect_main
        graphics::texture ldr_color;
        graphics::texture linear_depth; // read for cluster_lights sources
        graphics::texture_extent tile_size{ 1, 1 }; // pixels per cluster tile
        gpu::view_constants view_constants; // the camera's
        std::span<const debug_blit_source> blits;
        std::optional<debug_inspection> inspection;
    };

    struct debug_blit_outputs
    {
        graphics::texture ldr_color; // the version the blits wrote; the input's without blits
        graphics::buffer readback; // the version the inspector's copy wrote; invalid without inspection
    };

    // Adds the passes to the open view of `graph`: debug_blit when there are blits, the inspector's passes when there
    // is an inspection
    debug_blit_outputs add_debug_blits(graphics::render_graph& graph, const debug_blit_inputs& inputs);

    // Adds cluster_statistics and its readback copy for `cluster_lights` (cluster_count entries) to the open view
    graphics::buffer add_cluster_statistics(
        graphics::render_graph& graph,
        rhi::compute_pipeline_handle pipeline,
        graphics::buffer cluster_lights,
        graphics::buffer readback);
} // namespace avis::visualizer::rendering

#endif
