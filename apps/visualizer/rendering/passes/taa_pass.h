#ifndef AVIS_VISUALIZER_RENDERING_PASSES_TAA_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_TAA_PASS_H

// The TAA resolve (docs/design/11-renderer.md §2.3 pass 11, §10.2, docs/tasks/11-renderer.md §11.15), on the graphics
// queue's compute (taa.hlsl).
//
// taa_resolve: blends this frame's pre-exposed hdr_color with the view's history, reprojected by the dilated velocity
// and clipped to the neighbourhood's YCoCg variance box, into taa_output, the view's history slot of this frame. It
// also copies this frame's linear depth into the depth history, for the next frame's disocclusion test. Without a
// history (a new view, a resize, a reset, or a previous frame without TAA), taa_output is hdr_color unchanged and the
// previous slots aren't read.

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/pipeline.h"

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    // The resolve's fixed parameters (11 §10.2)
    inline constexpr float taa_blend = 0.1f; // this frame's base weight
    inline constexpr float taa_gamma = 1.0f; // the variance box's half-size, in standard deviations
    inline constexpr float taa_disocclusion_tolerance = 0.05f; // relative depth difference

    struct taa_inputs
    {
        rhi::compute_pipeline_handle pipeline; // taa.hlsl main
        graphics::texture hdr_color; // the forward pass's
        graphics::texture velocity; // gbuffer_velocity
        graphics::texture linear_depth;
        graphics::texture reactive_mask;
        graphics::texture history; // the view's slot of this frame, imported
        graphics::texture depth_history; // the same for the depth history
        graphics::texture previous_history; // the previous frame's slots; only read with has_history
        graphics::texture previous_depth_history;
        bool has_history = false;
        // The view's exposure buffer: with has_history, the history is rescaled by its current over its previous
        // exposure (exposure_passes.h)
        graphics::buffer exposure;
        gpu::view_constants view_constants;
    };

    struct taa_outputs
    {
        graphics::texture output; // taa_output: the version of `history` the pass wrote
        graphics::texture depth_history; // the version of `depth_history` the pass wrote
    };

    taa_outputs add_taa_resolve(graphics::render_graph& graph, const taa_inputs& inputs);
} // namespace avis::visualizer::rendering

#endif
