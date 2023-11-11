#ifndef RUNTIME_GRAPHICS_RENDER_PASS_BUILDER_H
#define RUNTIME_GRAPHICS_RENDER_PASS_BUILDER_H

#include "avis/runtime/graphics/resource.h"

namespace graphics
{
    class render_pass;
    class resource_graph;

    class render_pass_builder
    {
    public:
        ENGINE_API render_pass_builder(render_pass& pass, resource_graph& res_graph);

        ENGINE_API resource create_texture();
        ENGINE_API resource create_buffer();

        ENGINE_API resource read(resource& handle);
        ENGINE_API resource write(resource& handle);

    private:
        render_pass& render_pass_;
        resource_graph& resource_graph_;
    };
} // namespace graphics

#endif
