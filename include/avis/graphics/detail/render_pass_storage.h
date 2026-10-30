#ifndef AVIS_GRAPHICS_DETAIL_RENDER_PASS_STORAGE_H
#define AVIS_GRAPHICS_DETAIL_RENDER_PASS_STORAGE_H

// A render graph's pass node and the type-erased call of its execute callback (docs/design/10-render-graph.md §3.3,
// §3.5). Nodes live in the graph's linear_arena until the next reset(). Include render_graph.h.

#include "avis/graphics/linear_arena.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/resource.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace avis::graphics
{
    class resource_bag;

    namespace detail
    {
        inline constexpr std::uint32_t no_index = std::numeric_limits<std::uint32_t>::max();

        enum class resource_kind : std::uint8_t
        {
            texture,
            buffer
        };

        enum class access_direction : std::uint8_t
        {
            read,
            write
        };

        // One read or write a pass declared in its setup. Versions are indices into the graph's version table.
        struct declared_access
        {
            std::uint32_t resource = no_index;
            std::uint32_t input = no_index; // the version read, or the one a write overwrites
            std::uint32_t output = no_index; // writes: the version produced
            resource_kind kind = resource_kind::texture;
            access_direction direction = access_direction::read;
            std::uint8_t usage = 0; // the texture_read, texture_write, buffer_read or buffer_write value
            subresource_range range = all_subresources; // textures
            bool needs_clear = false; // texture writes declared with write_flags::needs_clear
        };

        template<typename Context>
        using execute_trampoline =
            void (*)(const void* callable, const void* data, const resource_bag& resources, Context& context);

        template<typename Callable, typename PassData, typename Context>
        void invoke_execute(
            const void* const callable, const void* const data, const resource_bag& resources, Context& context)
        {
            std::invoke(
                *static_cast<const Callable*>(callable), *static_cast<const PassData*>(data), resources, context);
        }

        struct render_pass_node
        {
            explicit render_pass_node(linear_arena& arena) : accesses{ arena_allocator<declared_access>{ arena } } {}

            std::string_view name; // qualified with the view's name; in the arena
            pass_type type = pass_type::graphics;
            compute_queue_hint queue_hint = compute_queue_hint::graphics;
            bool side_effects = false;
            std::uint32_t render_width = 0;
            std::uint32_t render_height = 0;

            const void* data = nullptr; // PassData, in the arena
            const void* callable = nullptr; // the decayed execute callable, in the arena
            // The one matching `type` is set
            execute_trampoline<graphics_pass_context> execute_graphics = nullptr;
            execute_trampoline<compute_pass_context> execute_compute = nullptr;
            execute_trampoline<copy_pass_context> execute_copy = nullptr;

            std::vector<declared_access, arena_allocator<declared_access>> accesses;

            std::span<const declared_access> declared_accesses() const noexcept
            {
                return accesses;
            }
        };
    } // namespace detail
} // namespace avis::graphics

#endif
