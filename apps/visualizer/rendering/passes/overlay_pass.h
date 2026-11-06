#ifndef AVIS_VISUALIZER_RENDERING_PASSES_OVERLAY_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_OVERLAY_PASS_H

// The overlays and the selection outline (docs/design/11-renderer.md §2.3 pass 14, §3.4, §10.5,
// docs/tasks/11-renderer.md §11.17), after the tonemap, into ldr_color:
//   selection_mask(_on_top)  graphics, on frames with a selected instance only: the runs of the camera's world and
//                            overlay groups that hold a selected instance, with each set's selection PSO (the overlay's
//                            depth test), then those of overlay_on_top with its selection_on_top PSO (no depth test),
//                            into selection_mask (R8_UNORM, view size, cleared to 0). The pixel shader discards the
//                            unselected instances of a run. Selected instances are few, so drawing them again costs
//                            less than an extra target in the G-buffer, forward and overlay passes would, and the mask
//                            of the overlays exists before the outline is drawn under them.
//   selection_outline        compute: the pixels within selection_outline_radius of the mask, outside it, become the
//                            selection colour
//   overlay                  graphics: scene/camera/overlay/opaque … subtractive with each set's overlay PSO, tested
//                            against the scene depth with overlay_depth_bias, without writing it
//   overlay_on_top           graphics: scene/camera/overlay_on_top/*, without a depth test
// Overlay colours are the materials' unlit colours, sRGB-encoded (overlay.hlsl): not exposed, not tonemapped.

#include "rendering/passes/scene_range_pass.h"
#include "rendering/range_walker.h"
#include "rendering/sort_key.h"

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/pipeline.h"
#include "avis/scene/types.h"

#include <span>

namespace avis::graphics
{
    class linear_arena;
} // namespace avis::graphics

namespace avis::visualizer::rendering
{
    // Linear; an orange that stands out from most scenes
    inline constexpr scene::color default_selection_color{ 1.0f, 0.35f, 0.0f, 1.0f };

    // The key ranges: scene/camera/overlay/opaque … subtractive, scene/camera/overlay_on_top/opaque … subtractive, and
    // the depth-tested ones selection_mask walks, scene/camera/world/opaque … scene/camera/overlay/subtractive
    key_range overlay_range() noexcept;
    key_range overlay_on_top_range() noexcept;
    key_range selection_range() noexcept;

    // The draw runs of `range` with at least one selected instance, in key order, in `arena`
    std::span<const draw_run>
        selected_runs(const prepared_draw_list& list, key_range range, graphics::linear_arena& arena);

    struct overlay_inputs
    {
        rhi::compute_pipeline_handle outline; // selection_outline.hlsl main
        graphics::texture ldr_color; // the tonemap's
        graphics::texture depth; // the scene's (the latest version)
        scene::color selection_color = default_selection_color; // linear; its alpha is ignored
    };

    struct overlay_outputs
    {
        graphics::texture ldr_color;
        graphics::texture depth; // the version the passes wrote (their PSOs write none)
        graphics::texture selection_mask; // invalid on a frame without a selected instance
    };

    overlay_outputs
        add_overlay(graphics::render_graph& graph, const scene_pass_inputs& inputs, const overlay_inputs& overlay);
} // namespace avis::visualizer::rendering

#endif
