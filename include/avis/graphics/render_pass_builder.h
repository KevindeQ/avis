#ifndef AVIS_GRAPHICS_RENDER_PASS_BUILDER_H
#define AVIS_GRAPHICS_RENDER_PASS_BUILDER_H

// What a pass's setup declares its resources through (docs/design/10-render-graph.md §3.1, §5). Bound to one pass
// while its setup runs inside render_graph::add_*_pass. Include render_graph.h.

#include "avis/graphics/buffer.h"
#include "avis/graphics/resource.h"
#include "avis/graphics/texture.h"

#include <cstdint>
#include <string_view>

namespace avis::graphics
{
    class render_graph;

    namespace detail
    {
        struct render_pass_node;
        enum class resource_kind : std::uint8_t;
    } // namespace detail

    class render_pass_builder
    {
    public:
        render_pass_builder(const render_pass_builder&) = delete;
        render_pass_builder& operator=(const render_pass_builder&) = delete;

        // Version 0 of a new transient resource, with this pass as its creator. The name is qualified with the open
        // view scope.
        texture create_texture(std::string_view name, const texture_description& description);
        buffer create_buffer(std::string_view name, const buffer_description& description);

        // Records a read of the given version, which makes its producer a dependency. Returns the handle unchanged.
        texture read(texture input, texture_read usage, subresource_range range = all_subresources);
        buffer read(buffer input, buffer_read usage);

        // The debug view's read of the resource selected in the debug UI (10 §9): a read like the ones above, so its
        // producers are kept, that also keeps the resource out of aliasing, so its memory holds only its own content
        texture read_for_debug(texture input, texture_read usage, subresource_range range = all_subresources);
        buffer read_for_debug(buffer input, buffer_read usage);

        // Records a write: the version passed in is consumed and a new one, produced by this pass, is returned. A
        // version may be written once; writing one that already has a successor fails a debug check (use the handle
        // the previous write returned). A partial first write of an aliased texture needs write_flags::needs_clear.
        texture write(
            texture output,
            texture_write usage,
            subresource_range range = all_subresources,
            write_flags flags = write_flags::none);
        buffer write(buffer output, buffer_write usage);

        // Never cull this pass
        void set_side_effects();
        void set_render_area(std::uint32_t width, std::uint32_t height);

    private:
        friend class render_graph;

        render_pass_builder(render_graph& graph, detail::render_pass_node& pass, std::uint32_t pass_index) noexcept :
            graph_{ graph },
            pass_{ pass },
            pass_index_{ pass_index }
        {}

        // False after a failed debug check
        bool record_read(resource handle, detail::resource_kind kind, std::uint8_t usage, subresource_range range);
        void select_for_debug(resource handle) noexcept;
        // The version-table index of the version produced, or detail::no_index after a failed debug check
        std::uint32_t record_write(
            resource handle,
            detail::resource_kind kind,
            std::uint8_t usage,
            subresource_range range,
            bool needs_clear = false);
        // Counts this pass as a reader of the version and records the edge from its producer
        void consume(std::uint32_t version);

        render_graph& graph_;
        detail::render_pass_node& pass_;
        std::uint32_t pass_index_;
    };
} // namespace avis::graphics

#endif
