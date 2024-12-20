#ifndef RENDER_GRAPH_RENDER_PASS_BUILDER_H
#define RENDER_GRAPH_RENDER_PASS_BUILDER_H

namespace render_graph
{
    class render_pass;
    class resource_graph;

    class render_pass_builder
    {
    public:
        render_pass_builder(render_pass& pass);

    private:
        render_pass& render_pass_;
    };
} // namespace render_graph

#endif
