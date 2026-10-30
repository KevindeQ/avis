#ifndef VISUALIZER_RENDERING_SCENE_RENDERER_H
#define VISUALIZER_RENDERING_SCENE_RENDERER_H

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/scene/scene.h"

namespace rendering
{
    // The Visualizer's renderer on the render graph (docs/design/10-render-graph.md, 11). So far a skeleton: it
    // declares the depth, linearise, G-buffer, ambient occlusion and lighting passes with their resources, and their
    // execute callbacks record nothing yet (the pipelines come with the renderer, 11).
    class scene_renderer
    {
    public:
        scene_renderer() = default;

        // Builds and compiles the frame's graph for one view of `view_size`. Executing it needs the frame chain of 12.
        // Nothing writes an import yet, so every pass is culled.
        void render(
            avis::graphics::render_graph& graph,
            avis::scene::scene& current_scene,
            avis::graphics::texture_extent view_size);

    private:
        void add_passes(avis::graphics::render_graph& graph, avis::scene::scene& current_scene);
    };
} // namespace rendering

#endif
