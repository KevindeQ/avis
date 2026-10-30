#ifndef AVIS_GRAPHICS_RENDER_GRAPH_EXECUTOR_H
#define AVIS_GRAPHICS_RENDER_GRAPH_EXECUTOR_H

// Private: the render graph's execution (docs/design/10-render-graph.md §7). So far the materialise step, which fills
// the resource bag, and the per-thread current pass the bag checks lookups against.

#include "render_graph_compiler.h"
#include "render_graph_tables.h"

#include "avis/error.h"
#include "avis/graphics/linear_arena.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/resources.h"

#include <span>
#include <type_traits>

namespace avis::graphics
{
    class frame_scheduler;
    class frame_token;
    class transient_resource_pool;
} // namespace avis::graphics

namespace avis::graphics::detail
{
    // The pass whose execute callback runs on this thread, or nullptr. Debug builds check bag lookups against its
    // declared accesses (§3.4).
    const render_pass_node* current_pass() noexcept;

    // Sets the current pass for the thread while alive; restores the previous one on destruction
    class current_pass_scope
    {
    public:
        explicit current_pass_scope(const render_pass_node* pass) noexcept;
        ~current_pass_scope();

        current_pass_scope(const current_pass_scope&) = delete;
        current_pass_scope& operator=(const current_pass_scope&) = delete;

    private:
        const render_pass_node* previous_;
    };

    struct resource_bag_access
    {
        static_assert(std::is_trivially_destructible_v<resource_bag>);

        // A bag in the arena; nothing to destroy
        static const resource_bag&
            create(linear_arena& arena, rhi::resources resources, std::span<const bag_entry> entries);
    };

    // §7 step 1: prepares the pool's heap for the frame's context, acquires the placed resources of the live
    // transient resources at their offsets, creates the views their accesses need in the frame's descriptor range,
    // and maps imports to their handles. Fails with the pool's or the scheduler's first error.
    result<const resource_bag*> materialise(
        const render_graph_tables& tables,
        const compiled_graph& compiled,
        transient_resource_pool& pool,
        frame_scheduler& frames,
        const frame_token& frame,
        rhi::resources resources,
        linear_arena& arena);
} // namespace avis::graphics::detail

#endif
