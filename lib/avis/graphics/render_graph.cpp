#include "avis/graphics/render_graph.h"

#include "render_graph_compiler.h"
#include "render_graph_tables.h"

#include "avis/detail/check.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace avis::graphics
{
    namespace
    {
        constexpr std::string_view logger_name = "graphics";
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
        configuration_{ configuration }
    {
        create_tables();
    }

    // The arena destroys the frame's passes, pass data and tables
    render_graph::~render_graph() = default;

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
        entry.initial_state = initial;
        entry.final_state = final;
        entry.ready_after = ready_after;
        entry.versions.push_back(static_cast<std::uint32_t>(tables.versions.size()));
        tables.versions.push_back(detail::version_entry{ .resource = index, .version = 0 });
        return detail::resource_handles::make<buffer>(index, 0, frame_serial_);
    }

    view_scope render_graph::begin_view(const std::string_view view_name)
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
        return view_scope{ this, stored };
    }

    void render_graph::reset()
    {
        AVIS_CHECK(logger_name, open_view_.empty(), "reset: a view scope is still open");
        compiled_ = nullptr;
        tables_ = nullptr;
        arena_.reset();
        ++frame_serial_;
        create_tables();
    }

    void render_graph::compile()
    {
        AVIS_CHECK(logger_name, open_view_.empty(), "compile: a view scope is still open");
        compiled_ = &detail::compile(*tables_, configuration_, arena_);
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
