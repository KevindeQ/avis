#include "render_graph_compiler.h"

#include "avis/detail/check.h"
#include "avis/graphics/buffer.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/barriers.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>

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

        // Per resource, the last batch of each queue that used it, and the layout its last use left a texture in
        const std::span<coverage> last_use = arena.allocate_array<coverage>(tables.resources.size());
        std::ranges::fill(last_use, nothing);
        const std::span<rhi::texture_layout> last_layout =
            arena.allocate_array<rhi::texture_layout>(tables.resources.size());
        for (std::size_t index = 0; index < tables.resources.size(); ++index)
        {
            const resource_entry& resource = tables.resources[index];
            last_layout[index] = resource.imported ? resource.initial_state.layout : rhi::texture_layout::undefined;
        }

        for (std::size_t batch = 0; batch < batch_count; ++batch)
        {
            submission_batch& entry = compiled.batches[batch];
            const std::size_t own = queue_slot(entry.queue);
            ready_offsets[batch] = static_cast<std::uint32_t>(ready_points.size());

            // The latest batch of each other queue this batch's passes depend on
            coverage required = nothing;
            for (const std::uint32_t pass : entry.passes)
            {
                const render_pass_node& node = *tables.passes[pass];
                for (const declared_access& access : node.declared_accesses())
                {
                    // Writes, and layout changes, wait for every other queue's earlier use: a queue may not overwrite
                    // or transition what another one may still read
                    bool exclusive = access.direction == access_direction::write;
                    if (access.kind == resource_kind::texture)
                    {
                        const rhi::texture_layout layout = usage_layout(access, node.type, entry.queue);
                        exclusive = exclusive || layout != last_layout[access.resource];
                        last_layout[access.resource] = layout;
                    }
                    if (exclusive)
                    {
                        for (std::size_t queue = 0; queue < queue_count; ++queue)
                        {
                            if (queue != own)
                            {
                                required[queue] = later(required[queue], last_use[access.resource][queue]);
                            }
                        }
                    }
                    last_use[access.resource][own] = static_cast<std::uint32_t>(batch);

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
        compiled.coverage = known;

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

    namespace
    {
        // ---- lifetimes ----

        constexpr std::uint16_t clamp_count(const std::uint32_t count) noexcept
        {
            return static_cast<std::uint16_t>(std::clamp<std::uint32_t>(count, 1, 0xFFFE));
        }

        // Mips and array layers of a texture (D3D12 subresources: a 3D texture has one layer, a cube six per cube)
        void subresource_counts(const resource_entry& entry, compiled_resource& resource) noexcept
        {
            const texture_description& description = entry.texture;
            const bool volume = description.dimension == rhi::texture_dimension::texture_3d;
            const std::uint32_t largest = std::max(
                { entry.extent.width, entry.extent.height, volume ? description.depth_or_layers : std::uint32_t{ 1 } });
            resource.mip_count = clamp_count(
                description.mip_levels == 0 ? static_cast<std::uint32_t>(std::bit_width(largest)) :
                                              description.mip_levels);
            switch (description.dimension)
            {
            case rhi::texture_dimension::texture_3d:
                resource.layer_count = 1;
                break;
            case rhi::texture_dimension::cube:
                resource.layer_count = clamp_count(description.depth_or_layers * 6);
                break;
            default:
                resource.layer_count = clamp_count(description.depth_or_layers);
                break;
            }
        }

        avis::flags<rhi::texture_usage> texture_usage_of(const declared_access& access) noexcept
        {
            using rhi::texture_usage;
            if (access.direction == access_direction::read)
            {
                switch (static_cast<texture_read>(access.usage))
                {
                case texture_read::srv_pixel:
                case texture_read::srv_compute:
                    return texture_usage::shader_resource;
                case texture_read::depth_read:
                    return texture_usage::depth_stencil | texture_usage::shader_resource;
                case texture_read::copy_source:
                    return texture_usage::copy_source;
                }
            }
            else
            {
                switch (static_cast<texture_write>(access.usage))
                {
                case texture_write::render_target:
                    return texture_usage::render_target;
                case texture_write::depth_write:
                    return texture_usage::depth_stencil;
                case texture_write::uav:
                    return texture_usage::unordered_access;
                case texture_write::copy_destination:
                    return texture_usage::copy_destination;
                }
            }
            return avis::flags<texture_usage>{ avis::empty };
        }

        avis::flags<rhi::buffer_usage> buffer_usage_of(const declared_access& access) noexcept
        {
            using rhi::buffer_usage;
            if (access.direction == access_direction::read)
            {
                switch (static_cast<buffer_read>(access.usage))
                {
                case buffer_read::srv_pixel:
                case buffer_read::srv_compute:
                    return buffer_usage::shader_resource;
                case buffer_read::indirect_arguments:
                    return buffer_usage::indirect_arguments;
                case buffer_read::copy_source:
                    return buffer_usage::copy_source;
                }
            }
            else
            {
                switch (static_cast<buffer_write>(access.usage))
                {
                case buffer_write::uav:
                    return buffer_usage::unordered_access;
                case buffer_write::copy_destination:
                    return buffer_usage::copy_destination;
                }
            }
            return avis::flags<buffer_usage>{ avis::empty };
        }

        struct batch_span
        {
            std::uint32_t first = 0; // positions in the order
            std::uint32_t last = 0;
        };

        // ---- placement ----

        constexpr std::uint64_t align_up(const std::uint64_t value, const std::uint64_t alignment) noexcept
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        struct occupied_range
        {
            std::uint64_t begin = 0;
            std::uint64_t end = 0;
        };

        // ---- barriers ----

        constexpr avis::flags<rhi::resource_access> read_accesses =
            rhi::resource_access::shader_resource | rhi::resource_access::depth_read |
            rhi::resource_access::copy_source | rhi::resource_access::indirect_argument |
            rhi::resource_access::vertex_buffer | rhi::resource_access::constant_buffer |
            rhi::resource_access::index_buffer;

        // Reads only; common (no bit) and no_access aren't
        constexpr bool read_only(const avis::flags<rhi::resource_access> access) noexcept
        {
            return !access.empty() && (access & ~read_accesses).empty();
        }

        constexpr bool same_state(const rhi::access_state& a, const rhi::access_state& b) noexcept
        {
            return a.synchronization == b.synchronization && a.access == b.access && a.layout == b.layout;
        }

        // Layouts a queue's command lists may use in a barrier
        constexpr bool layout_valid_on(const rhi::queue_type queue, const rhi::texture_layout layout) noexcept
        {
            using rhi::texture_layout;
            switch (queue)
            {
            case rhi::queue_type::graphics:
                return layout != texture_layout::compute_queue_common &&
                       layout != texture_layout::compute_queue_shader_resource &&
                       layout != texture_layout::compute_queue_unordered_access;
            case rhi::queue_type::compute:
                return layout == texture_layout::undefined || layout == texture_layout::common ||
                       layout == texture_layout::shader_resource || layout == texture_layout::unordered_access ||
                       layout == texture_layout::copy_source || layout == texture_layout::copy_destination ||
                       layout == texture_layout::compute_queue_common ||
                       layout == texture_layout::compute_queue_shader_resource ||
                       layout == texture_layout::compute_queue_unordered_access;
            case rhi::queue_type::copy:
                return layout == texture_layout::undefined || layout == texture_layout::common;
            }
            return false;
        }

        // The layout a texture is handed from one queue to another in (§6.3): the consumer's when both queues know it
        // and it is shader_resource or unordered_access, otherwise common
        constexpr rhi::texture_layout
            handoff_layout(const rhi::texture_layout wanted, const rhi::queue_type from, const rhi::queue_type to)
        {
            const bool shared =
                wanted == rhi::texture_layout::shader_resource || wanted == rhi::texture_layout::unordered_access;
            return shared && layout_valid_on(from, wanted) && layout_valid_on(to, wanted) ? wanted :
                                                                                            rhi::texture_layout::common;
        }

        rhi::access_state wanted_state(const declared_access& access, const pass_type type, const rhi::queue_type queue)
        {
            if (access.kind == resource_kind::texture)
            {
                return access.direction == access_direction::read ?
                           usage_access_state(static_cast<texture_read>(access.usage), type, queue) :
                           usage_access_state(static_cast<texture_write>(access.usage), type, queue);
            }
            return access.direction == access_direction::read ?
                       usage_access_state(static_cast<buffer_read>(access.usage), type, queue) :
                       usage_access_state(static_cast<buffer_write>(access.usage), type, queue);
        }

        // The state of a resource, or of one subresource, as the walk in order leaves it
        struct tracked_state
        {
            rhi::access_state state;
            std::uint32_t barrier = no_index; // the barrier that made this state, while reads may still merge into it
            std::uint32_t pass = no_index; // the last pass that used it
            std::uint32_t batch = no_index; // that pass's batch
            rhi::queue_type queue = rhi::queue_type::graphics;
            bool written = false; // the last use wrote it
        };

        constexpr bool same_tracking(const tracked_state& a, const tracked_state& b) noexcept
        {
            return same_state(a.state, b.state) && a.barrier == b.barrier && a.pass == b.pass && a.batch == b.batch &&
                   a.queue == b.queue && a.written == b.written;
        }

        // Barriers are collected with the list they go into, then grouped: pass p is owner p, the end of batch b is
        // pass_count + b, the trailing list of queue q is pass_count + batch_count + q
        struct owned_texture_transition
        {
            std::uint32_t owner = 0;
            texture_transition transition;
        };

        struct owned_buffer_transition
        {
            std::uint32_t owner = 0;
            buffer_transition transition;
        };

        enum class decision : std::uint8_t
        {
            none, // the state is already right
            merge, // a compatible read: its bits are added to the barrier that made the state
            barrier
        };

        // The barrier phase's working state
        class barrier_builder
        {
        public:
            barrier_builder(const render_graph_tables& tables, compiled_graph& compiled, linear_arena& arena) :
                tables_{ tables },
                compiled_{ compiled },
                arena_{ arena },
                textures_{ arena_allocator<owned_texture_transition>{ arena } },
                buffers_{ arena_allocator<owned_buffer_transition>{ arena } }
            {}

            void run()
            {
                const std::size_t resource_count = tables_.resources.size();
                whole_ = arena_.allocate_array<tracked_state>(resource_count);
                split_ = arena_.allocate_array<std::span<tracked_state>>(resource_count);
                mips_ = arena_.allocate_array<std::uint16_t>(resource_count);
                layers_ = arena_.allocate_array<std::uint16_t>(resource_count);
                initialise_states();

                for (const std::uint32_t pass : compiled_.order)
                {
                    const render_pass_node& node = *tables_.passes[pass];
                    const rhi::queue_type queue = compiled_.queues[pass];
                    const std::uint32_t batch = compiled_.pass_batches[pass];
                    for (const declared_access& access : node.declared_accesses())
                    {
                        const rhi::access_state wanted = wanted_state(access, node.type, queue);
                        const use current{ .pass = pass,
                                           .batch = batch,
                                           .queue = queue,
                                           .write = access.direction == access_direction::write };
                        if (access.kind == resource_kind::texture)
                        {
                            use_texture(access.resource, access.range, wanted, current);
                        }
                        else
                        {
                            use_whole(access.resource, false, wanted, current, pass);
                        }
                    }
                }

                finish_imports();
                group();
            }

        private:
            struct use
            {
                std::uint32_t pass = no_index;
                std::uint32_t batch = no_index;
                rhi::queue_type queue = rhi::queue_type::graphics;
                bool write = false;
            };

            std::uint32_t pass_count() const noexcept
            {
                return static_cast<std::uint32_t>(tables_.passes.size());
            }

            std::uint32_t batch_owner(const std::uint32_t batch) const noexcept
            {
                return pass_count() + batch;
            }

            std::uint32_t end_of_frame_owner(const rhi::queue_type queue) const noexcept
            {
                return pass_count() + static_cast<std::uint32_t>(compiled_.batches.size()) +
                       static_cast<std::uint32_t>(queue);
            }

            void initialise_states()
            {
                for (std::size_t index = 0; index < tables_.resources.size(); ++index)
                {
                    const resource_entry& entry = tables_.resources[index];
                    const compiled_resource& resource = compiled_.resources[index];
                    tracked_state state;
                    if (entry.imported)
                    {
                        state.state = entry.initial_state;
                    }
                    else
                    {
                        // Fresh memory: undefined content. Memory another resource used first waits for all earlier
                        // work, since that resource's last use isn't tracked here (§6.4 step 5).
                        state.state = { resource.aliased ? rhi::pipeline_synchronization::all :
                                                           rhi::pipeline_synchronization::none,
                                        rhi::resource_access::no_access,
                                        rhi::texture_layout::undefined };
                    }
                    whole_[index] = state;
                    split_[index] = {};

                    // Ranges beyond the counts (imports without a description) widen them
                    std::uint32_t mips = resource.mip_count;
                    std::uint32_t layers = resource.layer_count;
                    for (const std::uint32_t pass : compiled_.order)
                    {
                        for (const declared_access& access : tables_.passes[pass]->declared_accesses())
                        {
                            if (access.resource != index || access.kind != resource_kind::texture)
                            {
                                continue;
                            }
                            if (access.range.mip_count != subresource_range::all)
                            {
                                mips = std::max<std::uint32_t>(mips, access.range.first_mip + access.range.mip_count);
                            }
                            if (access.range.layer_count != subresource_range::all)
                            {
                                layers = std::max<std::uint32_t>(
                                    layers, access.range.first_layer + access.range.layer_count);
                            }
                        }
                    }
                    mips_[index] = clamp_count(mips);
                    layers_[index] = clamp_count(layers);
                }
            }

            // Decides how to move `tracked` to `wanted` for `current`; handoffs between queues are emitted here
            decision decide(
                tracked_state& tracked,
                const std::uint32_t resource,
                const bool texture,
                const subresource_range range,
                const rhi::access_state& wanted,
                const use& current)
            {
                if (tracked.pass != no_index && tracked.queue != current.queue)
                {
                    // The batches waited for each other (build_batches), so only the layout carries over. A texture
                    // written there, or in a layout this queue doesn't know, is transitioned on the queue that used
                    // it, at the end of its batch, before the signal.
                    if (texture && (tracked.written || !layout_valid_on(current.queue, tracked.state.layout)))
                    {
                        const rhi::texture_layout shared = handoff_layout(wanted.layout, tracked.queue, current.queue);
                        if (shared != tracked.state.layout)
                        {
                            textures_.push_back(
                                owned_texture_transition{
                                    .owner = batch_owner(tracked.batch),
                                    .transition = { .resource = resource,
                                                    .range = range,
                                                    .before = tracked.state,
                                                    .after = { rhi::pipeline_synchronization::none,
                                                               rhi::resource_access::no_access,
                                                               shared } } });
                            tracked.state.layout = shared;
                        }
                    }
                    tracked.state.synchronization = rhi::pipeline_synchronization::none;
                    tracked.state.access = rhi::resource_access::no_access;
                    tracked.barrier = no_index;
                    tracked.written = false;
                }

                const rhi::access_state& state = tracked.state;
                if (texture && state.layout != wanted.layout)
                {
                    return decision::barrier;
                }
                if (state.access == rhi::resource_access::no_access)
                {
                    // Fresh: nothing earlier on this queue to wait for, unless the memory was used before
                    return state.synchronization.empty() ? decision::none : decision::barrier;
                }
                if (tracked.pass == current.pass)
                {
                    return decision::merge;
                }
                if (read_only(state.access) && read_only(wanted.access))
                {
                    return decision::merge;
                }
                if (state.access == wanted.access && (wanted.access == rhi::resource_access::render_target ||
                                                      wanted.access == rhi::resource_access::depth_write))
                {
                    return decision::none; // render target and depth writes are ordered on one queue
                }
                return decision::barrier; // UAV after UAV included: an access-only barrier
            }

            // Applies the decision once the barrier (if any) is emitted at `barrier`
            static void apply(
                tracked_state& tracked,
                const decision kind,
                const rhi::access_state& wanted,
                const use& current,
                const std::uint32_t barrier)
            {
                const bool same_pass = tracked.pass == current.pass;
                switch (kind)
                {
                case decision::none:
                    tracked.state = wanted;
                    tracked.barrier = no_index;
                    break;
                case decision::merge:
                    tracked.state.synchronization |= wanted.synchronization;
                    tracked.state.access |= wanted.access;
                    break;
                case decision::barrier:
                    tracked.state = wanted;
                    tracked.barrier = barrier;
                    break;
                }
                tracked.written = current.write || (same_pass && tracked.written);
                tracked.pass = current.pass;
                tracked.batch = current.batch;
                tracked.queue = current.queue;
            }

            void merge_into(const tracked_state& tracked, const bool texture, const rhi::access_state& wanted)
            {
                if (tracked.barrier == no_index)
                {
                    return;
                }
                rhi::access_state& after =
                    texture ? textures_[tracked.barrier].transition.after : buffers_[tracked.barrier].transition.after;
                after.synchronization |= wanted.synchronization;
                after.access |= wanted.access;
            }

            // A whole-resource use: buffers, and textures not tracked per subresource
            void use_whole(
                const std::uint32_t resource,
                const bool texture,
                const rhi::access_state& wanted,
                const use& current,
                const std::uint32_t owner)
            {
                tracked_state& tracked = whole_[resource];
                const decision kind = decide(tracked, resource, texture, all_subresources, wanted, current);
                std::uint32_t barrier = no_index;
                if (kind == decision::merge)
                {
                    merge_into(tracked, texture, wanted);
                }
                else if (kind == decision::barrier)
                {
                    barrier = emit(resource, texture, all_subresources, tracked.state, wanted, owner);
                }
                apply(tracked, kind, wanted, current, barrier);
            }

            std::uint32_t emit(
                const std::uint32_t resource,
                const bool texture,
                const subresource_range range,
                const rhi::access_state& before,
                const rhi::access_state& after,
                const std::uint32_t owner)
            {
                if (texture)
                {
                    textures_.push_back(
                        owned_texture_transition{
                            .owner = owner,
                            .transition = { .resource = resource,
                                            .range = range,
                                            .before = before,
                                            .after = after,
                                            .discard = before.layout == rhi::texture_layout::undefined } });
                    return static_cast<std::uint32_t>(textures_.size() - 1);
                }
                buffers_.push_back(
                    owned_buffer_transition{
                        .owner = owner, .transition = { .resource = resource, .before = before, .after = after } });
                return static_cast<std::uint32_t>(buffers_.size() - 1);
            }

            void use_texture(
                const std::uint32_t resource,
                const subresource_range range,
                const rhi::access_state& wanted,
                const use& current)
            {
                const std::uint32_t mips = mips_[resource];
                const std::uint32_t layers = layers_[resource];
                const std::uint32_t first_mip = std::min<std::uint32_t>(range.first_mip, mips - 1);
                const std::uint32_t mip_end = range.mip_count == subresource_range::all ?
                                                  mips :
                                                  std::min<std::uint32_t>(mips, first_mip + range.mip_count);
                const std::uint32_t first_layer = std::min<std::uint32_t>(range.first_layer, layers - 1);
                const std::uint32_t layer_end = range.layer_count == subresource_range::all ?
                                                    layers :
                                                    std::min<std::uint32_t>(layers, first_layer + range.layer_count);
                const bool whole = first_mip == 0 && mip_end == mips && first_layer == 0 && layer_end == layers;

                if (split_[resource].empty())
                {
                    if (whole)
                    {
                        use_whole(resource, true, wanted, current, current.pass);
                        return;
                    }
                    split_[resource] = arena_.allocate_array<tracked_state>(std::size_t{ mips } * layers);
                    std::ranges::fill(split_[resource], whole_[resource]);
                }

                const std::span<tracked_state> subresources = split_[resource];
                for (std::uint32_t mip = first_mip; mip < mip_end; ++mip)
                {
                    // Consecutive layers of one mip moving from one state share a barrier
                    std::uint32_t run = no_index;
                    rhi::access_state run_before;
                    for (std::uint32_t layer = first_layer; layer < layer_end; ++layer)
                    {
                        tracked_state& tracked = subresources[std::size_t{ mip } * layers + layer];
                        const subresource_range single{ .first_mip = static_cast<std::uint16_t>(mip),
                                                        .mip_count = 1,
                                                        .first_layer = static_cast<std::uint16_t>(layer),
                                                        .layer_count = 1 };
                        const decision kind = decide(tracked, resource, true, single, wanted, current);
                        std::uint32_t barrier = no_index;
                        if (kind == decision::merge)
                        {
                            merge_into(tracked, true, wanted);
                        }
                        else if (kind == decision::barrier)
                        {
                            if (run != no_index && same_state(run_before, tracked.state))
                            {
                                ++textures_[run].transition.range.layer_count;
                                barrier = run;
                            }
                            else
                            {
                                run_before = tracked.state;
                                barrier = emit(resource, true, single, tracked.state, wanted, current.pass);
                                run = barrier;
                            }
                        }
                        if (kind != decision::barrier)
                        {
                            run = no_index;
                        }
                        apply(tracked, kind, wanted, current, barrier);
                    }
                }

                // Back to one state when every subresource agrees
                if (std::ranges::all_of(
                        subresources,
                        [&](const tracked_state& state) { return same_tracking(state, subresources.front()); }))
                {
                    whole_[resource] = subresources.front();
                    split_[resource] = {};
                }
            }

            // Imports end the frame in their final state, transitioned on the last queue that used them (§7 step 4)
            void finish_imports()
            {
                for (std::uint32_t resource = 0; resource < tables_.resources.size(); ++resource)
                {
                    const resource_entry& entry = tables_.resources[resource];
                    if (!entry.imported)
                    {
                        continue;
                    }
                    const bool texture = entry.kind == resource_kind::texture;
                    if (split_[resource].empty())
                    {
                        finish(resource, texture, all_subresources, whole_[resource], entry.final_state);
                        continue;
                    }
                    const std::uint32_t layers = layers_[resource];
                    for (std::uint32_t index = 0; index < split_[resource].size(); ++index)
                    {
                        const subresource_range single{ .first_mip = static_cast<std::uint16_t>(index / layers),
                                                        .mip_count = 1,
                                                        .first_layer = static_cast<std::uint16_t>(index % layers),
                                                        .layer_count = 1 };
                        finish(resource, texture, single, split_[resource][index], entry.final_state);
                    }
                }
            }

            void finish(
                const std::uint32_t resource,
                const bool texture,
                const subresource_range range,
                const tracked_state& tracked,
                const rhi::access_state& final)
            {
                rhi::queue_type queue = tracked.pass == no_index ? rhi::queue_type::graphics : tracked.queue;
                rhi::access_state before = tracked.state;
                if (texture && queue != rhi::queue_type::graphics && !layout_valid_on(queue, final.layout))
                {
                    // The graphics queue's trailing list makes the transition, after the last use
                    end_of_frame_list& list =
                        compiled_.end_of_frame[static_cast<std::size_t>(rhi::queue_type::graphics)];
                    std::uint32_t& wait = list.waits[static_cast<std::size_t>(queue)];
                    wait = wait == no_index ? tracked.batch : std::max(wait, tracked.batch);
                    compiled_.batches[tracked.batch].signals = true;
                    queue = rhi::queue_type::graphics;
                    before.synchronization = rhi::pipeline_synchronization::none;
                    before.access = rhi::resource_access::no_access;
                }

                const bool layout_matches = !texture || before.layout == final.layout;
                const bool access_matches =
                    before.access == final.access || (read_only(before.access) && read_only(final.access));
                if (layout_matches && access_matches)
                {
                    return;
                }
                emit(resource, texture, range, before, final, end_of_frame_owner(queue));
            }

            // Groups the collected barriers by owner into the compiled spans, keeping their order
            void group()
            {
                const std::size_t owners = tables_.passes.size() + compiled_.batches.size() + 3;
                const std::span<std::uint32_t> texture_starts = arena_.allocate_array<std::uint32_t>(owners + 1);
                const std::span<std::uint32_t> buffer_starts = arena_.allocate_array<std::uint32_t>(owners + 1);
                std::ranges::fill(texture_starts, 0u);
                std::ranges::fill(buffer_starts, 0u);
                for (const owned_texture_transition& entry : textures_)
                {
                    ++texture_starts[entry.owner + 1];
                }
                for (const owned_buffer_transition& entry : buffers_)
                {
                    ++buffer_starts[entry.owner + 1];
                }
                for (std::size_t owner = 0; owner < owners; ++owner)
                {
                    texture_starts[owner + 1] += texture_starts[owner];
                    buffer_starts[owner + 1] += buffer_starts[owner];
                }

                const std::span<texture_transition> textures =
                    arena_.allocate_array<texture_transition>(textures_.size());
                const std::span<buffer_transition> buffers = arena_.allocate_array<buffer_transition>(buffers_.size());
                {
                    const std::span<std::uint32_t> next = arena_.allocate_array<std::uint32_t>(owners);
                    std::ranges::copy(texture_starts.first(owners), next.begin());
                    for (const owned_texture_transition& entry : textures_)
                    {
                        textures[next[entry.owner]++] = entry.transition;
                    }
                    std::ranges::copy(buffer_starts.first(owners), next.begin());
                    for (const owned_buffer_transition& entry : buffers_)
                    {
                        buffers[next[entry.owner]++] = entry.transition;
                    }
                }

                const auto batch_of = [&](const std::size_t owner)
                {
                    return barrier_batch{ .textures = textures.subspan(
                                              texture_starts[owner], texture_starts[owner + 1] - texture_starts[owner]),
                                          .buffers = buffers.subspan(
                                              buffer_starts[owner], buffer_starts[owner + 1] - buffer_starts[owner]) };
                };
                compiled_.pass_barriers = arena_.allocate_array<barrier_batch>(tables_.passes.size());
                for (std::size_t pass = 0; pass < tables_.passes.size(); ++pass)
                {
                    compiled_.pass_barriers[pass] = batch_of(pass);
                }
                for (std::uint32_t batch = 0; batch < compiled_.batches.size(); ++batch)
                {
                    compiled_.batches[batch].end_barriers = batch_of(batch_owner(batch));
                }
                for (std::size_t queue = 0; queue < queue_count; ++queue)
                {
                    compiled_.end_of_frame[queue].barriers =
                        batch_of(end_of_frame_owner(static_cast<rhi::queue_type>(queue)));
                }
            }

            const render_graph_tables& tables_;
            compiled_graph& compiled_;
            linear_arena& arena_;
            arena_vector<owned_texture_transition> textures_;
            arena_vector<owned_buffer_transition> buffers_;
            std::span<tracked_state> whole_;
            std::span<std::span<tracked_state>> split_; // per resource; empty while tracked as a whole
            std::span<std::uint16_t> mips_;
            std::span<std::uint16_t> layers_;
        };
    } // namespace

    void compute_lifetimes(
        const render_graph_tables& tables,
        compiled_graph& compiled,
        allocation_requirements_source& requirements,
        linear_arena& arena)
    {
        compiled.resources = make_array<compiled_resource>(arena, tables.resources.size());
        for (std::size_t index = 0; index < tables.resources.size(); ++index)
        {
            const resource_entry& entry = tables.resources[index];
            compiled_resource& resource = compiled.resources[index];
            if (entry.kind == resource_kind::texture)
            {
                subresource_counts(entry, resource);
                resource.texture.usage = avis::flags<rhi::texture_usage>{ avis::empty };
            }
            resource.buffer.usage = avis::flags<rhi::buffer_usage>{ avis::empty };
        }

        // Uses in the order, and the usages they need
        for (std::uint32_t position = 0; position < compiled.order.size(); ++position)
        {
            const std::uint32_t pass = compiled.order[position];
            for (const declared_access& access : tables.passes[pass]->declared_accesses())
            {
                compiled_resource& resource = compiled.resources[access.resource];
                if (!resource.lifetime.live())
                {
                    resource.lifetime.first = position;
                    resource.first_pass = pass;
                }
                resource.lifetime.last = position;
                if (access.kind == resource_kind::texture)
                {
                    resource.texture.usage |= texture_usage_of(access);
                }
                else
                {
                    resource.buffer.usage |= buffer_usage_of(access);
                }
            }
        }

        // Batches outside the graphics queue run alongside other queues' work between the fences that bound them:
        // from after the last batch of each other queue they wait for, up to the first batch of each other queue that
        // waits for them (or the end of the frame)
        const std::size_t batch_count = compiled.batches.size();
        const auto span_of = [&](const std::size_t batch)
        {
            const std::span<const std::uint32_t> passes = compiled.batches[batch].passes;
            const auto first = static_cast<std::uint32_t>(passes.data() - compiled.order.data());
            return batch_span{ first, first + static_cast<std::uint32_t>(passes.size()) - 1 };
        };
        const auto last_position = static_cast<std::uint32_t>(compiled.order.size()) - 1;
        for (std::size_t batch = 0; batch < batch_count; ++batch)
        {
            const submission_batch& entry = compiled.batches[batch];
            if (entry.queue == rhi::queue_type::graphics)
            {
                continue;
            }
            const std::size_t own = queue_slot(entry.queue);
            batch_span window = span_of(batch);
            for (std::size_t queue = 0; queue < queue_count; ++queue)
            {
                if (queue == own)
                {
                    continue;
                }
                bool earlier = false;
                for (std::size_t other = 0; other < batch && !earlier; ++other)
                {
                    earlier = queue_slot(compiled.batches[other].queue) == queue;
                }
                if (earlier)
                {
                    const std::uint32_t waited = compiled.coverage[batch][queue];
                    window.first = std::min(window.first, waited == no_index ? 0 : span_of(waited).last + 1);
                }

                bool later_exists = false;
                std::uint32_t waiting = no_index;
                for (std::size_t other = batch + 1; other < batch_count && waiting == no_index; ++other)
                {
                    if (queue_slot(compiled.batches[other].queue) != queue)
                    {
                        continue;
                    }
                    later_exists = true;
                    if (covers(compiled.coverage[other][own], static_cast<std::uint32_t>(batch)))
                    {
                        waiting = static_cast<std::uint32_t>(other);
                    }
                }
                if (later_exists)
                {
                    window.last =
                        std::max(window.last, waiting == no_index ? last_position : span_of(waiting).first - 1);
                }
            }

            for (const std::uint32_t pass : entry.passes)
            {
                for (const declared_access& access : tables.passes[pass]->declared_accesses())
                {
                    if (tables.resources[access.resource].imported)
                    {
                        continue;
                    }
                    resource_lifetime& lifetime = compiled.resources[access.resource].lifetime;
                    lifetime.first = std::min(lifetime.first, window.first);
                    lifetime.last = std::max(lifetime.last, window.last);
                }
            }
        }

        // RHI descriptions and sizes of the live transient resources
        for (std::size_t index = 0; index < tables.resources.size(); ++index)
        {
            const resource_entry& entry = tables.resources[index];
            compiled_resource& resource = compiled.resources[index];
            if (entry.imported || !resource.lifetime.live())
            {
                continue;
            }
            if (entry.kind == resource_kind::texture)
            {
                const avis::flags<rhi::texture_usage> usage = resource.texture.usage;
                const bool target =
                    static_cast<bool>(usage & (rhi::texture_usage::render_target | rhi::texture_usage::depth_stencil));
                resource.texture =
                    rhi::texture_description{ .dimension = entry.texture.dimension,
                                              .width = entry.extent.width,
                                              .height = entry.extent.height,
                                              .depth_or_layers = entry.texture.depth_or_layers,
                                              .mip_levels = entry.texture.mip_levels,
                                              .format = entry.texture.format,
                                              .usage = usage,
                                              .optimized_clear = target ? entry.texture.clear_value : std::nullopt,
                                              .initial_layout = rhi::texture_layout::undefined,
                                              .debug_name = entry.name };
                resource.requirements = requirements.requirements(resource.texture);
            }
            else
            {
                resource.buffer =
                    rhi::buffer_description{ .size = entry.buffer.size,
                                             .structure_stride = entry.buffer.raw ? 0 : entry.buffer.stride,
                                             .usage = resource.buffer.usage,
                                             .memory = rhi::memory_type::gpu_only,
                                             .debug_name = entry.name };
                resource.requirements = requirements.requirements(resource.buffer);
            }
        }
    }

    placement place(const std::span<const placement_request> requests, const bool aliasing, linear_arena& arena)
    {
        const std::size_t count = requests.size();
        placement result;
        result.offsets = arena.allocate_array<std::uint64_t>(count);
        result.aliased = arena.allocate_array<std::uint8_t>(count);
        std::ranges::fill(result.aliased, std::uint8_t{ 0 });

        const auto alignment_of = [&](const std::size_t index)
        { return std::max<std::uint64_t>(requests[index].alignment, 1); };
        const auto occupied = [&](const std::size_t index)
        { return align_up(requests[index].size, alignment_of(index)); };
        // A request without aliasing lives the whole frame
        const auto overlaps = [&](const std::size_t a, const std::size_t b)
        {
            if (!aliasing || !requests[a].aliasable || !requests[b].aliasable)
            {
                return true;
            }
            return requests[a].first <= requests[b].last && requests[b].first <= requests[a].last;
        };

        const std::span<std::uint32_t> by_size = arena.allocate_array<std::uint32_t>(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            by_size[index] = index;
        }
        std::ranges::sort(
            by_size,
            [&](const std::uint32_t a, const std::uint32_t b)
            { return requests[a].size != requests[b].size ? requests[a].size > requests[b].size : a < b; });

        const std::span<occupied_range> taken = arena.allocate_array<occupied_range>(count);
        std::uint64_t heap_size = 0;
        for (std::size_t placed = 0; placed < count; ++placed)
        {
            const std::uint32_t index = by_size[placed];
            const std::uint64_t alignment = alignment_of(index);
            const std::uint64_t size = occupied(index);

            // The ranges of earlier requests alive at the same time, by offset; the lowest gap that fits wins
            std::size_t taken_count = 0;
            for (std::size_t earlier = 0; earlier < placed; ++earlier)
            {
                const std::uint32_t other = by_size[earlier];
                if (overlaps(index, other))
                {
                    taken[taken_count++] = { result.offsets[other], result.offsets[other] + occupied(other) };
                }
            }
            const std::span<occupied_range> live = taken.first(taken_count);
            std::ranges::sort(live, {}, &occupied_range::begin);
            std::uint64_t offset = 0;
            for (const occupied_range& range : live)
            {
                if (align_up(offset, alignment) + size <= range.begin)
                {
                    break;
                }
                offset = std::max(offset, range.end);
            }
            offset = align_up(offset, alignment);
            result.offsets[index] = offset;
            heap_size = std::max(heap_size, offset + size);
        }

        for (std::size_t a = 0; a < count; ++a)
        {
            for (std::size_t b = a + 1; b < count; ++b)
            {
                if (result.offsets[a] < result.offsets[b] + occupied(b) &&
                    result.offsets[b] < result.offsets[a] + occupied(a))
                {
                    result.aliased[a] = 1;
                    result.aliased[b] = 1;
                }
            }
        }

        result.statistics.heap_size = heap_size;
        for (std::size_t index = 0; index < count; ++index)
        {
            (result.aliased[index] != 0 ? result.statistics.aliased_bytes : result.statistics.non_aliased_bytes) +=
                occupied(index);
        }
        return result;
    }

    void place_resources(
        const render_graph_tables& tables,
        const render_graph_configuration& configuration,
        compiled_graph& compiled,
        linear_arena& arena)
    {
        std::size_t live = 0;
        for (std::size_t index = 0; index < tables.resources.size(); ++index)
        {
            live += !tables.resources[index].imported && compiled.resources[index].lifetime.live() ? 1 : 0;
        }
        const std::span<std::uint32_t> placed = arena.allocate_array<std::uint32_t>(live);
        const std::span<placement_request> requests = arena.allocate_array<placement_request>(live);
        {
            std::size_t next = 0;
            for (std::uint32_t index = 0; index < tables.resources.size(); ++index)
            {
                const compiled_resource& resource = compiled.resources[index];
                if (tables.resources[index].imported || !resource.lifetime.live())
                {
                    continue;
                }
                placed[next] = index;
                requests[next] = { .size = resource.requirements.size,
                                   .alignment = resource.requirements.alignment,
                                   .first = resource.lifetime.first,
                                   .last = resource.lifetime.last,
                                   .aliasable = tables.resources[index].aliasable };
                ++next;
            }
        }

        const placement result = place(requests, configuration.aliasing, arena);
        for (std::size_t request = 0; request < live; ++request)
        {
            compiled_resource& resource = compiled.resources[placed[request]];
            resource.offset = result.offsets[request];
            resource.aliased = result.aliased[request] != 0;
        }
        compiled.statistics = result.statistics;

        // A fresh aliased texture holds whatever the memory held: its first write must cover it, or clear it
        if (!configuration.aliasing)
        {
            return;
        }
        for (const std::uint32_t index : placed)
        {
            const resource_entry& entry = tables.resources[index];
            const compiled_resource& resource = compiled.resources[index];
            if (entry.kind != resource_kind::texture || !entry.aliasable)
            {
                continue;
            }
            const render_pass_node& node = *tables.passes[resource.first_pass];
            const auto first = std::ranges::find(node.declared_accesses(), index, &declared_access::resource);
            const subresource_range& range = first->range;
            const bool whole =
                range.first_mip == 0 && range.first_layer == 0 &&
                (range.mip_count == subresource_range::all || range.mip_count >= resource.mip_count) &&
                (range.layer_count == subresource_range::all || range.layer_count >= resource.layer_count);
            AVIS_CHECK(
                "graphics",
                first->direction != access_direction::write || whole || first->needs_clear,
                std::format(
                    "compile: pass '{}' writes only part of the fresh aliased texture '{}'; declare it with "
                    "write_flags::needs_clear",
                    node.name,
                    entry.name));
        }
    }

    void place_barriers(const render_graph_tables& tables, compiled_graph& compiled, linear_arena& arena)
    {
        barrier_builder{ tables, compiled, arena }.run();
    }

    compiled_graph& compile(
        const render_graph_tables& tables,
        const render_graph_configuration& configuration,
        allocation_requirements_source& requirements,
        linear_arena& arena)
    {
        compiled_graph& compiled = arena.create<compiled_graph>();
        cull(tables, compiled, arena);
        assign_queues(tables, configuration, compiled, arena);
        build_batches(tables, compiled, arena);
        compute_lifetimes(tables, compiled, requirements, arena);
        place_resources(tables, configuration, compiled, arena);
        place_barriers(tables, compiled, arena);
        return compiled;
    }
} // namespace avis::graphics::detail
