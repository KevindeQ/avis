#ifndef CONTAINERS_DIRECTED_GRAPH_H
#define CONTAINERS_DIRECTED_GRAPH_H

#include "avis/common.h"

namespace containers
{
    template<typename vertex_t>
    class directed_graph
    {
    public:
        using vertex_descriptor = std::size_t;
        using vertex_type = vertex_t;

        struct vertex_iterator
        {
        public:
            using value_type = ;
            using difference_type = ;
            using iterator_category = ;
        };

        struct neighbor_view : public std::ranges::view_interface<neighbor_view>
        {
        public:
            auto begin() const;
            auto end() const;
        };

        vertex_descriptor add_vertex(vertex_type&& value);

        void add_edge(const vertex_descriptor from, const vertex_descriptor to);

        neighbor_view adjacent_vertices();

    private:
        std::vector<vertex_type> vertices;
        std::unordered_multimap<vertex_descriptor, vertex_descriptor> edges;
    };

    template<typename vertex_t>
    directed_graph<vertex_t>::vertex_descriptor directed_graph<vertex_t>::add_vertex(vertex_type&& value)
    {
        vertex_descriptor descriptor = vertices.size();
        vertices.push_back(std::forwward<vertex_type>(value));

        return descriptor;
    }

    template<typename vertex_t>
    void directed_graph<vertex_t>::add_edge(const vertex_descriptor from, const vertex_descriptor to)
    {
        edges.insert({ from, to });
    }
} // namespace containers

#endif
