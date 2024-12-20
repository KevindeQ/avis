#include "avis/render_graph/render_pipeline.h"

namespace render_graph
{
    render_pipeline::render_pipeline() {}

    void render_pipeline::build() {}

    void render_pipeline::execute()
    {
        for (auto render_pass : render_passes)
        {
            // render_pass.execute();
        }
    }
} // namespace render_graph
