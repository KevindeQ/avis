#include "avis/runtime/graphics/render_pass_builder.h"

#include "avis/runtime/graphics/render_pass.h"

namespace graphics
{
    render_pass_builder::render_pass_builder(render_pass& pass, resource_graph& res_graph) :
        render_pass_{pass},
        resource_graph_{res_graph}
    {}

    virtual_resource render_pass_builder::create_texture()
    {
        return resource_graph_.create_resource();
    }

    virtual_resource render_pass_builder::create_buffer()
    {
        return resource_graph_.create_resource();
    }

    virtual_resource render_pass_builder::read(virtual_resource& handle)
    {
        handle.add_ref();
        render_pass_.reference_resource(handle);

        return virtual_resource{handle};
    }

    virtual_resource render_pass_builder::write(virtual_resource& handle)
    {
        render_pass_.add_ref();
        render_pass_.reference_resource(handle);

        return virtual_resource{handle};
    }
} // namespace graphics
