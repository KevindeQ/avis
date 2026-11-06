#ifndef AVIS_VISUALIZER_RENDERING_PASSES_SHADOW_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_SHADOW_PASS_H

// The sun's shadow cascades (docs/design/11-renderer.md §2.3 pass 7, §7, docs/tasks/11-renderer.md §11.12): one
// graphics pass (in recording parts) that walks scene/shadow_cascade[0 … count − 1]/world/opaque with each set's shadow
// PSO into shadow_map, a D32F Texture2DArray of count × resolution², cleared to 0 (reverse-Z: the far side). The key's
// viewport field selects the slice: cascade c draws into layer c with its own view constants
// (make_cascade_view_constants).

#include "rendering/cascades.h"
#include "rendering/passes/scene_range_pass.h"

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"

#include <cstdint>

namespace avis::visualizer::rendering
{
    // shadow_map, in the open view of `graph`
    graphics::texture add_shadow_cascades(
        graphics::render_graph& graph,
        const scene_pass_inputs& inputs,
        const shadow_cascades& cascades,
        std::uint32_t frame_index);
} // namespace avis::visualizer::rendering

#endif
