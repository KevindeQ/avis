#include "render_graph_compiler.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/barriers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace avis::graphics::detail
{
    namespace
    {
        constexpr std::size_t queue_count = 3;

        constexpr std::size_t queue_slot(const rhi::queue_type queue) noexcept
        {
            return static_cast<std::size_t>(queue);
        }

        // The later of two batch indices, where no_index means none
        constexpr std::uint32_t later(const std::uint32_t a, const std::uint32_t b) noexcept
        {
            if (a == no_index)
            {
                return b;
            }
            if (b == no_index)
            {
                return a;
            }
            return std::max(a, b);
        }

        // True when `known` covers `batch`: a batch at or after it on that queue was already waited for
        constexpr bool covers(const std::uint32_t known, const std::uint32_t batch) noexcept
        {
            return known != no_index && known >= batch;
        }

        rhi::texture_layout
            usage_layout(const declared_access& access, const pass_type type, const rhi::queue_type queue) noexcept
        {
            return access.direction == access_direction::read ?
                       usage_access_state(static_cast<texture_read>(access.usage), type, queue).layout :
                       usage_access_state(static_cast<texture_write>(access.usage), type, queue).layout;
        }
    } // namespace

    void cull(const render_graph_tables& tables, compiled_graph& compiled, linear_arena& arena)
    {
        const std::size_t pass_count = tables.passes.size();
        const std::size_t version_count = tables.versions.size();

        compiled.culled = arena.allocate_array<std::uint8_t>(pass_count);
        std::ranges::fill(compiled.culled, std::uint8_t{ 0 });

        // References: the versions a pass produces that someone reads, found by releasing the unread ones below
        const std::span<std::uint32_t> references = arena.allocate_array<std::uint32_t>(pass_count);
        const std::span<std::uint8_t> roots = arena.allocate_array<std::uint8_t>(pass_count);
        const std::span<std::uint32_t> readers = arena.allocate_array<std::uint32_t>(version_count);
        // Each version is pushed at most once: when it has no reader left
        const std::span<std::uint32_t> unread = arena.allocate_array<std::uint32_t>(version_count);
        std::size_t unread_count = 0;

        for (std::size_t version = 0; version < version_count; ++version)
        {
            const version_entry& entry = tables.versions[version];
            readers[version] = entry.readers;
            if (entry.readers == 0 && entry.producer != no_index)
            {
                unread[unread_count++] = static_cast<std::uint32_t>(version);
            }
        }

        for (std::size_t pass = 0; pass < pass_count; ++pass)
        {
            const render_pass_node& node = *tables.passes[pass];
            std::uint32_t writes = 0;
            bool root = node.side_effects;
            for (const declared_access& access : node.declared_accesses())
            {
                if (access.direction == access_direction::write)
                {
                    ++writes;
                    root = root || tables.resources[access.resource].imported;
                }
            }
            references[pass] = writes;
            roots[pass] = root ? 1 : 0;
        }

        const auto cull_pass = [&](const std::uint32_t pass)
        {
            compiled.culled[pass] = 1;
            for (const declared_access& access : tables.passes[pass]->declared_accesses())
            {
                const version_entry& input = tables.versions[access.input];
                // A pass reading its own output isn't counted as a reader (render_pass_builder::consume)
                if (input.producer == pass)
                {
                    continue;
                }
                if (--readers[access.input] == 0 && input.producer != no_index)
                {
                    unread[unread_count++] = access.input;
                }
            }
        };

        // Passes that write nothing and have no side effects contribute nothing
        for (std::uint32_t pass = 0; pass < pass_count; ++pass)
        {
            if (references[pass] == 0 && roots[pass] == 0)
            {
                cull_pass(pass);
            }
        }

        while (unread_count > 0)
        {
            const version_entry& entry = tables.versions[unread[--unread_count]];
            const std::uint32_t producer = entry.producer;
            if (compiled.culled[producer] != 0)
            {
                continue;
            }
            if (--references[producer] == 0 && roots[producer] == 0)
            {
                cull_pass(producer);
            }
        }

        const auto live = static_cast<std::size_t>(std::ranges::count(compiled.culled, std::uint8_t{ 0 }));
        compiled.order = arena.allocate_array<std::uint32_t>(live);
        std::size_t position = 0;
        for (std::uint32_t pass = 0; pass < pass_count; ++pass)
        {
            if (compiled.culled[pass] == 0)
            {
                compiled.order[position++] = pass;
            }
        }
    }

    void assign_queues(
        const render_graph_tables& tables,
        const render_graph_configuration& configuration,
        compiled_graph& compiled,
        linear_arena& arena)
    {
        compiled.queues = arena.allocate_array<rhi::queue_type>(tables.passes.size());
        std::ranges::fill(compiled.queues, rhi::queue_type::graphics);

        // The layout each texture is in before the pass being assigned: imports start in their initial layout,
        // transient textures are undefined until their first use
        const std::span<rhi::texture_layout> layouts =
            arena.allocate_array<rhi::texture_layout>(tables.resources.size());
        for (std::size_t index = 0; index < tables.resources.size(); ++index)
        {
            const resource_entry& entry = tables.resources[index];
            layouts[index] = entry.imported ? entry.initial_state.layout : rhi::texture_layout::undefined;
        }

        for (const std::uint32_t pass : compiled.order)
        {
            const render_pass_node& node = *tables.passes[pass];
            rhi::queue_type queue = rhi::queue_type::graphics;
            switch (node.type)
            {
            case pass_type::graphics:
                break;
            case pass_type::compute:
                if (node.queue_hint == compute_queue_hint::async_compute && configuration.async_compute)
                {
                    queue = rhi::queue_type::compute;
                }
                break;
            case pass_type::copy:
                // The copy queue only knows the common layout (§6.2, the §3.6 case)
                if (std::ranges::all_of(
                        node.declared_accesses(),
                        [&](const declared_access& access)
                        {
                            return access.kind == resource_kind::buffer ||
                                   layouts[access.resource] == rhi::texture_layout::common;
                        }))
                {
                    queue = rhi::queue_type::copy;
                }
                break;
            }
            compiled.queues[pass] = queue;

            for (const declared_access& access : node.declared_accesses())
            {
                if (access.kind == resource_kind::texture)
                {
                    layouts[access.resource] = usage_layout(access, node.type, queue);
                }
            }
        }
    }

    void build_batches(const render_graph_tables& tables, compiled_graph& compiled, linear_arena& arena)
    {
        compiled.pass_batches = arena.allocate_array<std::uint32_t>(tables.passes.size());
        std::ranges::fill(compiled.pass_batches, no_index);

        // Batches: runs of one queue in the order
        std::size_t batch_count = 0;
        for (std::size_t position = 0; position < compiled.order.size(); ++position)
        {
            if (position == 0 ||
                compiled.queues[compiled.order[position]] != compiled.queues[compiled.order[position - 1]])
            {
                ++batch_count;
            }
        }
        compiled.batches = arena.allocate_array<submission_batch>(batch_count);
        {
            std::size_t batch = 0;
            std::size_t start = 0;
            for (std::size_t position = 0; position <= compiled.order.size(); ++position)
            {
                const bool ends = position == compiled.order.size() ||
                                  (position > start &&
                                   compiled.queues[compiled.order[position]] != compiled.queues[compiled.order[start]]);
                if (!ends)
                {
                    continue;
                }
                if (position > start)
                {
                    submission_batch& entry = compiled.batches[batch];
                    entry = submission_batch{};
                    entry.queue = compiled.queues[compiled.order[start]];
                    entry.passes = compiled.order.subspan(start, position - start);
                    for (const std::uint32_t pass : entry.passes)
                    {
                        compiled.pass_batches[pass] = static_cast<std::uint32_t>(batch);
                    }
                    ++batch;
                }
                start = position;
            }
        }

        // Per queue, the latest batch of every queue its submissions so far have waited for (itself included); and
        // per batch, that knowledge once it has run. Waiting for a batch implies everything it waited for.
        using coverage = std::array<std::uint32_t, queue_count>;
        constexpr coverage nothing{ no_index, no_index, no_index };
        std::array<coverage, queue_count> covered{ nothing, nothing, nothing };
        const std::span<coverage> known = arena.allocate_array<coverage>(batch_count);

        const std::span<std::uint32_t> waits = arena.allocate_array<std::uint32_t>(batch_count * (queue_count - 1));
        std::size_t wait_count = 0;

        arena_vector<rhi::synchronization_point> ready_points{ arena_allocator<rhi::synchronization_point>{ arena } };
        const std::span<std::uint32_t> ready_offsets = arena.allocate_array<std::uint32_t>(batch_count + 1);
        // Per import, a bit per queue that has waited for its ready_after point
        const std::span<std::uint8_t> import_waited = arena.allocate_array<std::uint8_t>(tables.resources.size());
        std::ranges::fill(import_waited, std::uint8_t{ 0 });

        for (std::size_t batch = 0; batch < batch_count; ++batch)
        {
            submission_batch& entry = compiled.batches[batch];
            const std::size_t own = queue_slot(entry.queue);
            ready_offsets[batch] = static_cast<std::uint32_t>(ready_points.size());

            // The latest batch of each other queue this batch's passes depend on
            coverage required = nothing;
            for (const std::uint32_t pass : entry.passes)
            {
                for (const declared_access& access : tables.passes[pass]->declared_accesses())
                {
                    const std::uint32_t producer = tables.versions[access.input].producer;
                    if (producer != no_index && producer != pass)
                    {
                        const std::uint32_t producer_batch = compiled.pass_batches[producer];
                        const std::size_t producer_queue = queue_slot(compiled.batches[producer_batch].queue);
                        if (producer_queue != own)
                        {
                            required[producer_queue] = later(required[producer_queue], producer_batch);
                        }
                    }

                    const resource_entry& resource = tables.resources[access.resource];
                    const auto queue_bit = static_cast<std::uint8_t>(1u << own);
                    if (resource.imported && resource.ready_after.has_value() && resource.ready_after->value != 0 &&
                        (import_waited[access.resource] & queue_bit) == 0)
                    {
                        import_waited[access.resource] |= queue_bit;
                        ready_points.push_back(*resource.ready_after);
                    }
                }
            }

            // Waits not implied by earlier waits of this queue, nor by another wait of this batch
            std::array<bool, queue_count> candidate{};
            for (std::size_t queue = 0; queue < queue_count; ++queue)
            {
                candidate[queue] = required[queue] != no_index && !covers(covered[own][queue], required[queue]);
            }
            const std::size_t first_wait = wait_count;
            for (std::size_t queue = 0; queue < queue_count; ++queue)
            {
                if (!candidate[queue])
                {
                    continue;
                }
                bool implied = false;
                for (std::size_t other = 0; other < queue_count; ++other)
                {
                    implied = implied || (other != queue && candidate[other] &&
                                          covers(known[required[other]][queue], required[queue]));
                }
                if (!implied)
                {
                    waits[wait_count++] = required[queue];
                    compiled.batches[required[queue]].signals = true;
                }
            }
            for (std::size_t wait = first_wait; wait < wait_count; ++wait)
            {
                for (std::size_t queue = 0; queue < queue_count; ++queue)
                {
                    covered[own][queue] = later(covered[own][queue], known[waits[wait]][queue]);
                }
            }
            entry.waits = waits.subspan(first_wait, wait_count - first_wait);

            covered[own][own] = static_cast<std::uint32_t>(batch);
            known[batch] = covered[own];
        }
        ready_offsets[batch_count] = static_cast<std::uint32_t>(ready_points.size());

        // Copied out of the vector, whose storage the arena keeps but whose elements end with it
        const std::span<rhi::synchronization_point> all_ready =
            arena.allocate_array<rhi::synchronization_point>(ready_points.size());
        std::ranges::copy(ready_points, all_ready.begin());
        for (std::size_t batch = 0; batch < batch_count; ++batch)
        {
            compiled.batches[batch].ready_after =
                all_ready.subspan(ready_offsets[batch], ready_offsets[batch + 1] - ready_offsets[batch]);
        }
    }

    compiled_graph&
        compile(const render_graph_tables& tables, const render_graph_configuration& configuration, linear_arena& arena)
    {
        compiled_graph& compiled = arena.create<compiled_graph>();
        cull(tables, compiled, arena);
        assign_queues(tables, configuration, compiled, arena);
        build_batches(tables, compiled, arena);
        return compiled;
    }
} // namespace avis::graphics::detail
