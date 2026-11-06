#ifndef AVIS_VISUALIZER_RENDERING_PASSES_PICKING_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_PICKING_PASS_H

// Picking (docs/design/11-renderer.md §5.1, docs/tasks/11-renderer.md §11.19), on a click frame only, after the
// overlays:
//   picking          graphics: scene/camera/world/opaque … scene/camera/overlay/subtractive with each set's picking PSO
//                    (the overlay's depth test against the scene depth, no blending), into picking_id (R32G32_UINT,
//                    view size, cleared to 0)
//   picking_on_top   graphics: scene/camera/overlay_on_top/* with the picking_on_top PSO (no depth test)
//   pick_window      compute: the pick_window_size² texels around the click into a small buffer
//   pick_readback    copy: that buffer into the readback buffer the renderer imported
// Key order is draw order, so what is drawn later wins, as in the image: overlays over transparents over opaque
// surfaces, transparents back to front. picking.hlsl discards the instances that aren't pickable and the pixels covered
// less than picking_minimum_coverage. The colour passes are unchanged, so a click frame's image is a normal frame's.

#include "rendering/passes/scene_range_pass.h"
#include "rendering/picking.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/pipeline.h"

namespace avis::visualizer::rendering
{
    struct picking_inputs
    {
        rhi::compute_pipeline_handle window; // pick_window.hlsl main
        graphics::texture depth; // the scene's (the latest version)
        pick_request request;
        graphics::buffer readback; // imported; at least pick_window_bytes, written from offset 0
    };

    struct picking_outputs
    {
        graphics::texture picking_id;
        graphics::texture depth; // the version the passes wrote (their PSOs write none)
        graphics::buffer readback; // the version the copy wrote
    };

    picking_outputs
        add_picking(graphics::render_graph& graph, const scene_pass_inputs& inputs, const picking_inputs& picking);
} // namespace avis::visualizer::rendering

#endif
