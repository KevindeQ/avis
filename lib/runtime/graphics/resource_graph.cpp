#include "avis/runtime/graphics/resource_graph.h"

#include "avis/runtime/graphics/render_pass.h"

namespace graphics
{
    resource resource_graph::create_resource()
    {
        return resource(0);
    }

    void resource_graph::allocate_resource(resource handle) {}

    void resource_graph::deallocate_resource(resource handle) {}
} // namespace graphics
