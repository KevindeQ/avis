#ifndef AVIS_GRAPHICS_RENDER_GRAPH_COMPILER_H
#define AVIS_GRAPHICS_RENDER_GRAPH_COMPILER_H

// Private: the render graph's compiler (docs/design/10-render-graph.md §6). Pure: it reads the build tables and writes
// a compiled_graph into the arena, and calls the RHI for nothing. One function per phase, run in order by compile().

#include "render_graph_tables.h"

#include "avis/graphics/linear_arena.h"
#include "avis/graphics/render_graph.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <span>

namespace avis::graphics::detail
{
    // Consecutive passes on one queue, submitted together (§6.2)
    struct submission_batch
    {
        rhi::queue_type queue = rhi::queue_type::graphics;
        std::span<const std::uint32_t> passes; // pass indices, in order; a subspan of compiled_graph::order
        std::span<const std::uint32_t> waits; // earlier batches on other queues to wait for, at most one per queue
        std::span<const rhi::synchronization_point> ready_after; // imports' points, the first use on this queue
        bool signals = false; // a later batch on another queue waits for this one
    };

    // All spans point into the graph's arena, valid until the next reset()
    struct compiled_graph
    {
        std::span<std::uint8_t> culled; // per pass, 1 when culled
        std::span<std::uint32_t> order; // the passes that run, in declaration order
        std::span<rhi::queue_type> queues; // per pass; culled passes keep graphics
        std::span<std::uint32_t> pass_batches; // per pass, its batch; no_index when culled
        std::span<submission_batch> batches; // in submission order
    };

    // Reference-counted culling (§6.1): passes with side effects or writing an import are roots; a version nobody
    // reads releases its producer. Fills culled and order.
    void cull(const render_graph_tables& tables, compiled_graph& compiled, linear_arena& arena);

    // §6.2: async-hinted compute passes go to the compute queue when enabled; copy passes go to the copy queue when
    // every texture they touch is in the common layout at that point and everything else is a buffer
    void assign_queues(
        const render_graph_tables& tables,
        const render_graph_configuration& configuration,
        compiled_graph& compiled,
        linear_arena& arena);

    // Consecutive passes on one queue form a batch. A cross-queue dependency makes the consumer's batch wait for the
    // producer's, which signals; waits already implied by earlier ones are left out. On each queue, the first batch
    // using an import with a ready_after point waits for it.
    void build_batches(const render_graph_tables& tables, compiled_graph& compiled, linear_arena& arena);

    // The phases in order
    compiled_graph& compile(
        const render_graph_tables& tables, const render_graph_configuration& configuration, linear_arena& arena);
} // namespace avis::graphics::detail

#endif
