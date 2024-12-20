#include "avis/render_graph/render_pass_builder.h"

#include "avis/render_graph/render_pass.h"

namespace render_graph
{
    render_pass_builder::render_pass_builder(render_pass& pass) :
        render_pass_{ pass }
    {}
} // namespace render_graph
