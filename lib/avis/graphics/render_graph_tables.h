#ifndef AVIS_GRAPHICS_RENDER_GRAPH_TABLES_H
#define AVIS_GRAPHICS_RENDER_GRAPH_TABLES_H

// Private: the render graph's build tables (docs/tasks/09-render-graph.md, Conventions), shared by the graph, its
// builder, the compiler and the executor. Flat arrays in the graph's linear_arena, indexed by std::uint32_t, rebuilt
// every frame.

#include "avis/graphics/buffer.h"
#include "avis/graphics/detail/render_pass_storage.h"
#include "avis/graphics/linear_arena.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace avis::graphics::detail
{
    template<typename T>
    using arena_vector = std::vector<T, arena_allocator<T>>;

    // Default-constructed array in the arena, for types that aren't trivially copyable (allocate_array) but need no
    // destructor
    template<typename T>
    std::span<T> make_array(linear_arena& arena, const std::size_t count)
    {
        static_assert(std::is_trivially_destructible_v<T>, "make_array registers no destructor");
        T* const first = static_cast<T*>(arena.allocate(count * sizeof(T), alignof(T)));
        std::uninitialized_value_construct_n(first, count);
        return { first, count };
    }

    struct resource_entry
    {
        explicit resource_entry(linear_arena& arena) : versions{ arena_allocator<std::uint32_t>{ arena } } {}

        std::string_view name; // qualified; in the arena
        resource_kind kind = resource_kind::texture;
        bool imported = false;
        bool aliasable = true; // transient resources may share memory (not for the debug selection, 9.10)
        // Textures. For imports with a valid handle, built from the RHI's description at import.
        texture_description texture;
        texture_extent extent; // the size, resolved against the view size at creation
        buffer_description buffer; // buffers

        // Imports
        rhi::texture_handle texture_handle;
        rhi::buffer_handle buffer_handle;
        rhi::access_state initial_state;
        rhi::access_state final_state;
        std::optional<rhi::synchronization_point> ready_after;

        std::uint32_t creator = no_index; // the pass that created a transient resource
        arena_vector<std::uint32_t> versions; // version-table indices, by version number
    };

    // One version of a resource (§5): produced by one write, read by any number of passes
    struct version_entry
    {
        std::uint32_t resource = no_index;
        std::uint32_t version = 0;
        std::uint32_t producer = no_index; // the pass that wrote it; none for version 0
        std::uint32_t readers = 0; // reads plus writes that overwrite it, by passes other than its producer
        std::uint32_t successor = no_index; // the version a write made from it
    };

    // A dependency between two passes through one version
    struct dependency_edge
    {
        std::uint32_t producer = no_index;
        std::uint32_t consumer = no_index;
        std::uint32_t version = no_index;
    };

    struct render_graph_tables
    {
        explicit render_graph_tables(linear_arena& arena) :
            passes{ arena_allocator<render_pass_node*>{ arena } },
            resources{ arena_allocator<resource_entry>{ arena } },
            versions{ arena_allocator<version_entry>{ arena } },
            edges{ arena_allocator<dependency_edge>{ arena } },
            view_names{ arena_allocator<std::string_view>{ arena } }
        {}

        arena_vector<render_pass_node*> passes; // in declaration order; nodes in the arena
        arena_vector<resource_entry> resources;
        arena_vector<version_entry> versions;
        arena_vector<dependency_edge> edges;
        arena_vector<std::string_view> view_names; // the views begun this frame
    };
} // namespace avis::graphics::detail

#endif
