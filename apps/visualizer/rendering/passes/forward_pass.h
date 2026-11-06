#ifndef AVIS_VISUALIZER_RENDERING_PASSES_FORWARD_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_FORWARD_PASS_H

// The forward transparent pass (docs/design/11-renderer.md §2.3 pass 10, §10.1, docs/tasks/11-renderer.md §11.14).
//
// forward_transparent: the ranges scene/camera/world/blended … subtractive, back to front (11 §4.3), with each pipeline
// set's forward PSO. It tests depth against the prepass's depth without writing it, and blends premultiplied colour
// into hdr_color after the sky. Lit materials are shaded with the lights, clusters, sun shadow and baked environment
// deferred_lighting uses, without GTAO. It also writes reactive_mask (R8_UNORM, view size, cleared to 0), the most
// reactive transparent of each pixel, for TAA (11.15). The pass is added even when no transparent is drawn, so
// reactive_mask always exists.

#include "rendering/environment_baker.h"
#include "rendering/passes/lighting_passes.h"
#include "rendering/passes/scene_range_pass.h"

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"

#include <cstdint>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    struct forward_inputs
    {
        graphics::texture hdr_color; // the sky's
        graphics::texture depth; // the prepass's (the latest version)
        const light_clusters& clusters;
        std::uint32_t sun = gpu::no_light; // index in the frame's lights
        const environment_resources& environment;
        float exposure = 1.0f;
        float ambient_intensity = 1.0f;
        graphics::texture shadow_map; // invalid: no shadows, everything is lit
        gpu::shadow_constants shadow{}; // the cascades of shadow_map; its shadow_map index is set when recording
    };

    struct forward_outputs
    {
        graphics::texture hdr_color;
        graphics::texture reactive_mask; // R8_UNORM, view size
        graphics::texture depth; // the version the pass wrote (its PSOs write none)
    };

    // The key range forward_transparent walks: scene/camera/world/blended … subtractive
    key_range forward_range() noexcept;

    forward_outputs add_forward_transparent(
        graphics::render_graph& graph, const scene_pass_inputs& inputs, const forward_inputs& forward);
} // namespace avis::visualizer::rendering

#endif
