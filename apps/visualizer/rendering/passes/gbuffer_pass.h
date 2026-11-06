#ifndef AVIS_VISUALIZER_RENDERING_PASSES_GBUFFER_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_GBUFFER_PASS_H

// The G-buffer pass (docs/design/11-renderer.md §2.3 pass 4, §5, docs/tasks/11-renderer.md §11.10).
//
// gbuffer: the range scene/camera/world/opaque with each pipeline set's G-buffer PSO, depth EQUAL against the
// prepass's depth and no depth writes, into five targets cleared to 0:
//   gbuffer_albedo    RGBA8_UNORM_SRGB  base colour, material AO in A
//   gbuffer_normal    RG16_UNORM        world-space normal, octahedral
//   gbuffer_material  RGBA8_UNORM       metallic, roughness, shading model, flags
//   gbuffer_emissive  R11G11B10_FLOAT   emissive radiance
//   velocity          RG16_FLOAT        current minus previous unjittered UV

#include "rendering/passes/scene_range_pass.h"

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"

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
} // namespace avis::visualizer::rendering

#endif
