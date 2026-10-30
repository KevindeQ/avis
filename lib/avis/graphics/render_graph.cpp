#include "avis/graphics/render_graph.h"

#include "render_graph_compiler.h"
#include "render_graph_executor.h"
#include "render_graph_tables.h"

#include "avis/detail/check.h"
#include "avis/graphics/error.h"
#include "avis/graphics/transient_resource_pool.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace avis::graphics
{
    namespace
    {
        constexpr std::string_view logger_name = "graphics";

        // The compiler's allocation sizes, from the pool's cache of rhi::memory::requirements
        class pool_requirements final : public detail::allocation_requirements_source
        {
        public:
            explicit pool_requirements(transient_resource_pool& pool) noexcept : pool_{ pool } {}

            rhi::allocation_requirements requirements(const rhi::texture_description& description) override
            {
                return pool_.requirements(description);
            }

            rhi::allocation_requirements requirements(const rhi::buffer_description& description) override
            {
                return pool_.requirements(description);
            }

        private:
            transient_resource_pool& pool_;
        };

        // The debug UI's view of a compiled frame (§9), in the arena
        graph_debug_information& build_debug_information(
            const detail::render_graph_tables& tables, const detail::compiled_graph& compiled, linear_arena& arena)
        {
            graph_debug_information& information = arena.create<graph_debug_information>();
            information.transient_heap_size = compiled.statistics.heap_size;
            information.aliased_bytes = compiled.statistics.aliased_bytes;
            information.non_aliased_bytes = compiled.statistics.non_aliased_bytes;

            const std::span<pass_debug_information> passes =
                detail::make_array<pass_debug_information>(arena, tables.passes.size());
            for (std::size_t pass = 0; pass < passes.size(); ++pass)
            {
                const detail::render_pass_node& node = *tables.passes[pass];
                passes[pass] = { .name = node.name,
                                 .type = node.type,
                                 .queue = compiled.queues[pass],
                                 .culled = compiled.culled[pass] != 0 };
            }
            information.passes = passes;

            // Producers and consumers: counted, then filled; a pass is listed once per resource and role
            const std::size_t resource_count = tables.resources.size();
            const std::span<std::uint32_t> producer_counts = arena.allocate_array<std::uint32_t>(resource_count);
            const std::span<std::uint32_t> consumer_counts = arena.allocate_array<std::uint32_t>(resource_count);
            std::ranges::fill(producer_counts, 0u);
            std::ranges::fill(consumer_counts, 0u);
            const std::span<std::uint32_t> last_producer = arena.allocate_array<std::uint32_t>(resource_count);
            const std::span<std::uint32_t> last_consumer = arena.allocate_array<std::uint32_t>(resource_count);
            const auto count_roles = [&](const auto& visit)
            {
                std::ranges::fill(last_producer, detail::no_index);
                std::ranges::fill(last_consumer, detail::no_index);
                for (std::uint32_t pass = 0; pass < tables.passes.size(); ++pass)
                {
                    for (const detail::declared_access& access : tables.passes[pass]->declared_accesses())
                    {
                        const bool write = access.direction == detail::access_direction::write;
                        std::uint32_t& last = write ? last_producer[access.resource] : last_consumer[access.resource];
                        if (last != pass)
                        {
                            last = pass;
                            visit(access.resource, pass, write);
                        }
                    }
                }
            };
            count_roles([&](const std::uint32_t resource, std::uint32_t, const bool write)
                        { ++(write ? producer_counts : consumer_counts)[resource]; });

            const std::span<resource_debug_information> resources =
                detail::make_array<resource_debug_information>(arena, resource_count);
            const std::span<std::span<std::uint32_t>> producers =
                arena.allocate_array<std::span<std::uint32_t>>(resource_count);
            const std::span<std::span<std::uint32_t>> consumers =
                arena.allocate_array<std::span<std::uint32_t>>(resource_count);
            for (std::size_t index = 0; index < resource_count; ++index)
            {
                const detail::resource_entry& entry = tables.resources[index];
                const detail::compiled_resource& compiled_resource = compiled.resources[index];
                resource_debug_information& resource = resources[index];
                resource.name = entry.name;
                resource.is_texture = entry.kind == detail::resource_kind::texture;
                resource.texture = entry.texture;
                resource.texture.size = absolute(entry.extent.width, entry.extent.height);
                resource.buffer = entry.buffer;
                resource.imported = entry.imported;
                resource.debug_selected = entry.debug_selected;
                resource.heap_offset = compiled_resource.offset;
                resource.aliased = compiled_resource.aliased;
                resource.first_use = compiled_resource.lifetime.first;
                resource.last_use = compiled_resource.lifetime.last;
                producers[index] = arena.allocate_array<std::uint32_t>(producer_counts[index]);
                consumers[index] = arena.allocate_array<std::uint32_t>(consumer_counts[index]);
                resource.producers = producers[index];
                resource.consumers = consumers[index];
            }
            std::ranges::fill(producer_counts, 0u);
            std::ranges::fill(consumer_counts, 0u);
            count_roles(
                [&](const std::uint32_t resource, const std::uint32_t pass, const bool write)
                {
                    std::uint32_t& next = (write ? producer_counts : consumer_counts)[resource];
                    (write ? producers : consumers)[resource][next++] = pass;
                });
            information.resources = resources;
            return information;
        }
    } // namespace

    render_graph::render_graph(
        const rhi::resources resources,
        const rhi::command_lists command_lists,
        const rhi::queue<rhi::queue_type::graphics> graphics_queue,
        const rhi::queue<rhi::queue_type::compute> compute_queue,
        const rhi::queue<rhi::queue_type::copy> copy_queue,
        transient_resource_pool& pool,
        frame_scheduler& frames,
        gpu_profiler* const profiler,
        const render_graph_configuration& configuration) :
        resources_{ resources },
        command_lists_{ command_lists },
        graphics_queue_{ graphics_queue },
        compute_queue_{ compute_queue },
        copy_queue_{ copy_queue },
        pool_{ pool },
        frames_{ frames },
        profiler_{ profiler },
        configuration_{ configuration },
        executor_{ std::make_unique<detail::render_graph_executor>(*this) }
    {
        create_tables();
    }

    // The arena destroys the frame's passes, pass data and tables
    render_graph::~render_graph() = default;

    const graph_debug_information& render_graph::debug_information() const noexcept
    {
        static const graph_debug_information empty;
        return debug_information_ != nullptr ? *debug_information_ : empty;
    }

    texture render_graph::import_texture(
        const std::string_view name,
        const rhi::texture_handle texture_handle,
        const rhi::access_state initial,
        const rhi::access_state final,
        const std::optional<rhi::synchronization_point> ready_after)
    {
        detail::render_graph_tables& tables = *tables_;
        const auto index = static_cast<std::uint32_t>(tables.resources.size());
        detail::resource_entry& entry = tables.resources.emplace_back(arena_);
        entry.name = qualify(name);
        entry.kind = detail::resource_kind::texture;
        entry.imported = true;
        entry.texture_handle = texture_handle;
        if (texture_handle.is_valid())
        {
            // Sizes and subresource counts for the barriers and the bag
            const rhi::texture_description& description = resources_.description(texture_handle);
            entry.texture = texture_description{ .size = absolute(description.width, description.height),
                                                 .depth_or_layers = description.depth_or_layers,
                                                 .mip_levels = description.mip_levels,
                                                 .format = description.format,
                                                 .dimension = description.dimension,
                                                 .clear_value = description.optimized_clear };
            entry.extent = { description.width, description.height };
        }
        entry.initial_state = initial;
        entry.final_state = final;
        entry.ready_after = ready_after;
        entry.versions.push_back(static_cast<std::uint32_t>(tables.versions.size()));
        tables.versions.push_back(detail::version_entry{ .resource = index, .version = 0 });
        return detail::resource_handles::make<texture>(index, 0, frame_serial_);
    }

    buffer render_graph::import_buffer(
        const std::string_view name,
        const rhi::buffer_handle buffer_handle,
        const rhi::access_state initial,
        const rhi::access_state final,
        const std::optional<rhi::synchronization_point> ready_after)
    {
        detail::render_graph_tables& tables = *tables_;
        const auto index = static_cast<std::uint32_t>(tables.resources.size());
        detail::resource_entry& entry = tables.resources.emplace_back(arena_);
        entry.name = qualify(name);
        entry.kind = detail::resource_kind::buffer;
        entry.imported = true;
        entry.buffer_handle = buffer_handle;
        if (buffer_handle.is_valid())
        {
            const rhi::buffer_description& description = resources_.description(buffer_handle);
            entry.buffer = buffer_description{ .size = description.size,
                                               .stride = description.structure_stride,
                                               .raw = description.structure_stride == 0 };
        }
        entry.initial_state = initial;
        entry.final_state = final;
        entry.ready_after = ready_after;
        entry.versions.push_back(static_cast<std::uint32_t>(tables.versions.size()));
        tables.versions.push_back(detail::version_entry{ .resource = index, .version = 0 });
        return detail::resource_handles::make<buffer>(index, 0, frame_serial_);
    }

    view_scope render_graph::begin_view(const std::string_view view_name)
    {
        return begin_view(view_name, default_view_size_);
    }

    view_scope render_graph::begin_view(const std::string_view view_name, const texture_extent view_size)
    {
        if (!AVIS_CHECK(logger_name, open_view_.empty(), "begin_view: a view scope is already open (no nesting)"))
        {
            return view_scope{ nullptr, {} };
        }
        if (!AVIS_CHECK(
                logger_name,
                !view_name.empty() && !view_name.contains('/'),
                "begin_view: a view name must be non-empty and contain no '/'"))
        {
            return view_scope{ nullptr, {} };
        }
        // A repeated name only makes names collide; the scope still opens
        AVIS_CHECK(
            logger_name,
            std::ranges::find(tables_->view_names, view_name) == tables_->view_names.end(),
            "begin_view: this view name was already used this frame");

        const std::string_view stored = qualify(view_name); // no scope is open: a plain copy
        tables_->view_names.push_back(stored);
        open_view_ = stored;
        open_view_size_ = view_size;
        return view_scope{ this, stored };
    }

    void render_graph::reset()
    {
        AVIS_CHECK(logger_name, open_view_.empty(), "reset: a view scope is still open");
        compiled_ = nullptr;
        debug_information_ = nullptr;
        tables_ = nullptr;
        arena_.reset();
        ++frame_serial_;
        create_tables();
    }

    void render_graph::compile()
    {
        AVIS_CHECK(logger_name, open_view_.empty(), "compile: a view scope is still open");
        pool_requirements requirements{ pool_ };
        compiled_ = &detail::compile(*tables_, configuration_, requirements, arena_);
        debug_information_ = &build_debug_information(*tables_, *compiled_, arena_);
    }

    result<const resource_bag*> render_graph::materialise(const frame_token& frame)
    {
        if (!AVIS_CHECK(logger_name, compiled_ != nullptr, "materialise: compile the graph first"))
        {
            return std::unexpected(make_error(error_kind::invalid_argument, "render_graph: not compiled"));
        }
        return detail::materialise(*tables_, *compiled_, pool_, frames_, frame, resources_, arena_);
    }

    std::uint32_t
        render_graph::begin_pass(const std::string_view name, const pass_type type, const compute_queue_hint queue)
    {
        detail::render_pass_node& node = arena_.create<detail::render_pass_node>(arena_);
        node.name = qualify(name);
        node.type = type;
        node.queue_hint = queue;
        const auto index = static_cast<std::uint32_t>(tables_->passes.size());
        tables_->passes.push_back(&node);
        return index;
    }

    detail::render_pass_node& render_graph::pass(const std::uint32_t index) noexcept
    {
        return *tables_->passes[index];
    }

    std::string_view render_graph::qualify(const std::string_view name)
    {
        const std::size_t prefix = open_view_.empty() ? 0 : open_view_.size() + 1;
        const std::span<char> characters = arena_.allocate_array<char>(prefix + name.size());
        if (prefix > 0)
        {
            std::ranges::copy(open_view_, characters.begin());
            characters[open_view_.size()] = '/';
        }
        std::ranges::copy(name, characters.begin() + static_cast<std::ptrdiff_t>(prefix));
        return { characters.data(), characters.size() };
    }

    void render_graph::end_view() noexcept
    {
        open_view_ = {};
    }

    void render_graph::create_tables()
    {
        tables_ = &arena_.create<detail::render_graph_tables>(arena_);
        open_view_ = {};
    }

    view_scope::~view_scope()
    {
        if (graph_ != nullptr)
        {
            graph_->end_view();
        }
    }
} // namespace avis::graphics
