#include "avis/runtime/graphics/render_pass_builder.h"

#include "avis/runtime/graphics/render_pass.h"

namespace graphics
{
    render_pass_builder::render_pass_builder(render_pass& pass, resource_graph& res_graph) :
        render_pass_{pass},
        resource_graph_{res_graph}
    {}

    resource render_pass_builder::create_texture()
    {
        return resource_graph_.create_resource();
    }

    resource render_pass_builder::create_buffer()
    {
        return resource_graph_.create_resource();
    }

    resource render_pass_builder::read(resource& handle)
    {
        handle.add_ref();
        render_pass_.reference_resource(handle);

        return resource{handle};
    }

    resource render_pass_builder::write(resource& handle)
    {
        render_pass_.add_ref();
        render_pass_.reference_resource(handle);

        return resource{handle};
    }
} // namespace graphics
