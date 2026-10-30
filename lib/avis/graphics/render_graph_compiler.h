#ifndef AVIS_GRAPHICS_RENDER_GRAPH_COMPILER_H
#define AVIS_GRAPHICS_RENDER_GRAPH_COMPILER_H

// Private: the render graph's compiler (docs/design/10-render-graph.md §6). Pure: it reads the build tables and writes
// a compiled_graph into the arena, and calls the RHI for nothing. Allocation sizes come in through
// allocation_requirements_source, which the graph binds to the transient pool and tests replace with fixed sizes. One
// function per phase, run in order by compile().

#include "render_graph_tables.h"

#include "avis/graphics/linear_arena.h"
#include "avis/graphics/render_graph.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace avis::graphics::detail
{
    inline constexpr std::uint64_t no_offset = std::numeric_limits<std::uint64_t>::max();

    // A barrier on a virtual resource; the executor fills in the physical handle from the resource bag
    struct texture_transition
    {
        std::uint32_t resource = no_index;
        subresource_range range = all_subresources;
        rhi::access_state before;
        rhi::access_state after;
        bool discard = false; // the first use of a transient texture (§6.4 step 5)
    };

    struct buffer_transition
    {
        std::uint32_t resource = no_index;
        rhi::access_state before;
        rhi::access_state after;
    };

    // One barrier() call (§6.3)
    struct barrier_batch
    {
        std::span<const texture_transition> textures;
        std::span<const buffer_transition> buffers;

        bool empty() const noexcept
        {
            return textures.empty() && buffers.empty();
        }
    };

    // Consecutive passes on one queue, submitted together (§6.2)
    struct submission_batch
    {
        rhi::queue_type queue = rhi::queue_type::graphics;
        std::span<const std::uint32_t> passes; // pass indices, in order; a subspan of compiled_graph::order
        std::span<const std::uint32_t> waits; // earlier batches on other queues to wait for, at most one per queue
        std::span<const rhi::synchronization_point> ready_after; // imports' points, the first use on this queue
        bool signals = false; // a later batch on another queue waits for this one
        // Recorded after the batch's last pass, before its signal: transitions of resources handed to another queue,
        // into a layout valid on both (§6.3)
        barrier_batch end_barriers;
    };

    // The trailing list of one queue (§7 step 4): imports' transitions to their final state
    struct end_of_frame_list
    {
        barrier_batch barriers;
        // Per queue, a batch this list waits for (no_index for none): the last use of an import whose final layout
        // isn't valid on the queue that used it last, so the graphics list makes the transition
        std::array<std::uint32_t, 3> waits{ no_index, no_index, no_index };
    };

    // Positions in compiled_graph::order (§6.4 step 1)
    struct resource_lifetime
    {
        std::uint32_t first = no_index; // no_index: no pass that runs uses the resource
        std::uint32_t last = no_index;

        bool live() const noexcept
        {
            return first != no_index;
        }
    };

    struct memory_statistics
    {
        std::uint64_t heap_size = 0; // the transient heap this frame needs
        std::uint64_t aliased_bytes = 0; // aligned sizes of transient resources sharing memory with another
        std::uint64_t non_aliased_bytes = 0; // aligned sizes of transient resources with memory of their own
    };

    // Per resource
    struct compiled_resource
    {
        resource_lifetime lifetime; // transient: the uses, widened for async batches; imports: the uses
        std::uint32_t first_pass = no_index; // the first pass that runs and uses it
        std::uint16_t mip_count = 1; // subresources; 1 × 1 for imports without a valid handle
        std::uint16_t layer_count = 1;
        // Live transient resources only
        rhi::allocation_requirements requirements;
        std::uint64_t offset = no_offset; // in the frame's transient heap
        bool aliased = false; // shares memory with another resource
        rhi::texture_description texture; // the RHI description: resolved size, usages from the accesses
        rhi::buffer_description buffer;
    };

    // All spans point into the graph's arena, valid until the next reset()
    struct compiled_graph
    {
        std::span<std::uint8_t> culled; // per pass, 1 when culled
        std::span<std::uint32_t> order; // the passes that run, in declaration order
        std::span<rhi::queue_type> queues; // per pass; culled passes keep graphics
        std::span<std::uint32_t> pass_batches; // per pass, its batch; no_index when culled
        std::span<submission_batch> batches; // in submission order
        // Per batch and queue, the latest batch of that queue the batch waits for, directly or through others (its
        // own index for its own queue)
        std::span<std::array<std::uint32_t, 3>> coverage;
        std::span<compiled_resource> resources; // per resource
        std::span<barrier_batch> pass_barriers; // per pass, recorded before it; empty for culled passes
        std::array<end_of_frame_list, 3> end_of_frame; // by queue
        memory_statistics statistics;
    };

    // Allocation sizes for the RHI descriptions of transient resources (rhi::memory::requirements)
    class allocation_requirements_source
    {
    public:
        virtual rhi::allocation_requirements requirements(const rhi::texture_description& description) = 0;
        virtual rhi::allocation_requirements requirements(const rhi::buffer_description& description) = 0;

    protected:
        ~allocation_requirements_source() = default;
    };

    // One transient resource to place (§6.4 step 2)
    struct placement_request
    {
        std::uint64_t size = 0;
        std::uint64_t alignment = 1; // a power of two
        std::uint32_t first = 0; // lifetime, inclusive
        std::uint32_t last = 0;
        bool aliasable = true; // false: a range of its own
    };

    struct placement
    {
        std::span<std::uint64_t> offsets; // per request
        std::span<std::uint8_t> aliased; // per request, 1 when its range overlaps another request's
        memory_statistics statistics;
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
    // producer's, which signals: reading a version produced on another queue, writing a resource another queue used,
    // or changing the layout of a texture another queue used. Waits already implied by earlier ones are left out. On
    // each queue, the first batch using an import with a ready_after point waits for it.

    void build_batches(const render_graph_tables& tables, compiled_graph& compiled, linear_arena& arena);

    // Lifetimes, subresource counts and RHI descriptions (§6.4 step 1): [first, last] use in the order, widened for
    // passes on the compute and copy queues to the span between the fences that bound their batch, so their memory
    // isn't shared with work that may run at the same time. Asks `requirements` for each live transient resource.
    void compute_lifetimes(
        const render_graph_tables& tables,
        compiled_graph& compiled,
        allocation_requirements_source& requirements,
        linear_arena& arena);

    // Greedy interval placement in one heap (§6.4 step 2): by size, descending (ties by index), each request takes
    // the lowest aligned offset not occupied by a request whose lifetime overlaps its own. With aliasing off, or for a
    // request that isn't aliasable, its range is its own. Deterministic.
    placement place(std::span<const placement_request> requests, bool aliasing, linear_arena& arena);

    // Places the live transient resources with place() and fills their offsets and the statistics. A partial first
    // write of an aliased texture without needs_clear fails a debug check naming the pass and the resource.
    void place_resources(
        const render_graph_tables& tables,
        const render_graph_configuration& configuration,
        compiled_graph& compiled,
        linear_arena& arena);

    // Barriers (§6.3): walks each resource's accesses in order and emits transitions into the consuming pass's batch,
    // handoffs between queues at the end of the producer's batch, and imports' final transitions into the trailing
    // list of the last queue that used them. Subresources are tracked per mip and layer once a partial range is used.
    void place_barriers(const render_graph_tables& tables, compiled_graph& compiled, linear_arena& arena);

    // The phases in order: cull, assign_queues, build_batches, compute_lifetimes, place_resources, place_barriers
    // (barriers need the placement: the first use of memory another resource used waits for all earlier work)
    compiled_graph& compile(
        const render_graph_tables& tables,
        const render_graph_configuration& configuration,
        allocation_requirements_source& requirements,
        linear_arena& arena);
} // namespace avis::graphics::detail

#endif
