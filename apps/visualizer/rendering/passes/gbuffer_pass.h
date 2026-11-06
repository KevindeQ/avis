#ifndef AVIS_VISUALIZER_RENDERING_PASSES_GBUFFER_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_GBUFFER_PASS_H

// The G-buffer pass (docs/design/11-renderer.md §2.3 pass 4, §5, docs/tasks/11-renderer.md §11.10), and the placeholder
// that shows it until lighting and tonemapping exist.
//
// gbuffer: the range scene/camera/world/opaque with each pipeline set's G-buffer PSO, depth EQUAL against the
// prepass's depth and no depth writes, into five targets cleared to 0:
//   gbuffer_albedo    RGBA8_UNORM_SRGB  base colour, material AO in A
//   gbuffer_normal    RG16_UNORM        world-space normal, octahedral
//   gbuffer_material  RGBA8_UNORM       metallic, roughness, shading model, flags
//   gbuffer_emissive  R11G11B10_FLOAT   emissive radiance
//   velocity          RG16_FLOAT        current minus previous unjittered UV
//
// gbuffer_preview (compute, named "tonemap" until 11.16 replaces it): ldr_color is the base colour, sRGB-encoded, where
// linear_depth is finite, and background_color elsewhere.

#include "rendering/passes/scene_range_pass.h"

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/pipeline.h"

#include <array>

namespace avis::visualizer::rendering
{
    struct gbuffer_targets
    {
        graphics::texture albedo;
        graphics::texture normal;
        graphics::texture material;
        graphics::texture emissive;
        graphics::texture velocity;
        graphics::texture depth; // the version the pass wrote (it writes none: EQUAL, no writes)
    };

    gbuffer_targets
        add_gbuffer_pass(graphics::render_graph& graph, const scene_pass_inputs& inputs, graphics::texture depth);

    // ldr_color, RGBA8_UNORM
    graphics::texture add_gbuffer_preview(
        graphics::render_graph& graph,
        rhi::compute_pipeline_handle pipeline,
        const gbuffer_targets& gbuffer,
        graphics::texture linear_depth,
        const std::array<float, 4>& background);
} // namespace avis::visualizer::rendering

#endif
