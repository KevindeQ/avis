#ifndef RUNTIME_GRAPHICS_RESOURCE_GRAPH_H
#define RUNTIME_GRAPHICS_RESOURCE_GRAPH_H

#include "avis/runtime/graphics/resource.h"
#include "avis/runtime/graphics/resource_graph.h"

namespace graphics
{
    class resource_graph
    {
    public:
        resource_graph() = default;
        resource_graph(const resource_graph&) = delete;
        resource_graph(resource_graph&&) = delete;
        ~resource_graph() = default;

        resource create_resource();

        void allocate_resource(resource handle);
        void deallocate_resource(resource handle);

        resource_graph& operator=(const resource_graph&) = delete;
        resource_graph& operator=(resource_graph&&) = delete;

    private:
        std::vector<resource_descriptor> descriptors_;
        std::mutex descriptors_mutex_;
    };
} // namespace graphics

#endif
