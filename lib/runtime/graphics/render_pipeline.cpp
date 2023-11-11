#include "avis/runtime/graphics/render_pipeline.h"

namespace graphics
{
    render_pipeline::render_pipeline(resource_graph& resources) : resource_graph_{resources} {}

    void render_pipeline::build() {}

    void render_pipeline::execute()
    {
        for (auto render_pass : render_passes_)
        {
            // render_pass.execute();
        }
    }
} // namespace graphics
