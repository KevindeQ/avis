#ifndef AVIS_GRAPHICS_VIEW_SCOPE_H
#define AVIS_GRAPHICS_VIEW_SCOPE_H

// A named scope for one camera view (docs/design/10-render-graph.md §8.1): while it's alive, the names of passes,
// resources and imports created through the graph get the "<view name>/" prefix. It only affects naming. Made by
// render_graph::begin_view; include render_graph.h.

#include <string_view>

namespace avis::graphics
{
    class render_graph;

    class view_scope
    {
    public:
        view_scope(view_scope&& other) noexcept : graph_{ other.graph_ }, name_{ other.name_ }
        {
            other.graph_ = nullptr;
        }

        view_scope& operator=(view_scope&&) = delete;
        view_scope(const view_scope&) = delete;
        view_scope& operator=(const view_scope&) = delete;

        // Ends the scope
        ~view_scope();

        // Empty for a scope begin_view refused (a failed debug check), which qualifies nothing
        std::string_view name() const noexcept
        {
            return name_;
        }

    private:
        friend class render_graph;

        view_scope(render_graph* graph, const std::string_view name) noexcept : graph_{ graph }, name_{ name } {}

        render_graph* graph_;
        std::string_view name_; // in the graph's arena
    };
} // namespace avis::graphics

#endif
