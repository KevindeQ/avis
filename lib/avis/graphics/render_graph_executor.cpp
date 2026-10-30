#include "render_graph_executor.h"

#include "avis/detail/check.h"
#include "avis/graphics/error.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/gpu_profiler.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/transient_resource_pool.h"
#include "avis/threading/parallel_helpers.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

namespace avis::graphics::detail
{
    namespace
    {
        // The views a transient resource's accesses need
        struct view_needs
        {
            bool srv = false;
            bool uav = false; // buffers
            std::uint64_t uav_mips = 0; // textures: a bit per mip (the first 64)
        };

        void add_needs(view_needs& needs, const declared_access& access, const std::uint32_t mip_count) noexcept
        {
            if (access.kind == resource_kind::texture)
            {
                if (access.direction == access_direction::read)
                {
                    const auto usage = static_cast<texture_read>(access.usage);
                    needs.srv = needs.srv || usage == texture_read::srv_pixel || usage == texture_read::srv_compute ||
                                usage == texture_read::depth_read;
                }
                else if (static_cast<texture_write>(access.usage) == texture_write::uav)
                {
                    const std::uint32_t first = access.range.first_mip;
                    const std::uint32_t end =
                        access.range.mip_count == subresource_range::all ? mip_count : first + access.range.mip_count;
                    for (std::uint32_t mip = first; mip < end && mip < 64; ++mip)
                    {
                        needs.uav_mips |= std::uint64_t{ 1 } << mip;
                    }
                }
                return;
            }
            if (access.direction == access_direction::read)
            {
                const auto usage = static_cast<buffer_read>(access.usage);
                needs.srv = needs.srv || usage == buffer_read::srv_pixel || usage == buffer_read::srv_compute;
            }
            else
            {
                needs.uav = needs.uav || static_cast<buffer_write>(access.usage) == buffer_write::uav;
            }
        }
    } // namespace

    result<const resource_bag*> materialise(
        const render_graph_tables& tables,
        const compiled_graph& compiled,
        transient_resource_pool& pool,
        frame_scheduler& frames,
        const frame_token& frame,
        const rhi::resources resources,
        linear_arena& arena)
    {
        const std::uint32_t context_index = frame.context_index();
        if (auto prepared = pool.prepare(context_index, compiled.statistics.heap_size); !prepared)
        {
            return std::unexpected(std::move(prepared.error()));
        }

        const std::size_t resource_count = tables.resources.size();
        const std::span<view_needs> needs = arena.allocate_array<view_needs>(resource_count);
        std::ranges::fill(needs, view_needs{});
        for (const std::uint32_t pass : compiled.order)
        {
            for (const declared_access& access : tables.passes[pass]->declared_accesses())
            {
                add_needs(needs[access.resource], access, compiled.resources[access.resource].mip_count);
            }
        }

        const std::span<bag_entry> entries = make_array<bag_entry>(arena, resource_count);
        for (std::uint32_t index = 0; index < resource_count; ++index)
        {
            const resource_entry& source = tables.resources[index];
            const compiled_resource& resource = compiled.resources[index];
            bag_entry& entry = entries[index];
            entry.kind = source.kind;
            entry.imported = source.imported;
            entry.texture_info = source.texture;
            entry.texture_info.size = absolute(source.extent.width, source.extent.height);
            entry.buffer_info = source.buffer;

            if (source.imported)
            {
                entry.physical_texture = source.texture_handle;
                entry.physical_buffer = source.buffer_handle;
                if (source.kind == resource_kind::buffer && source.buffer_handle.is_valid())
                {
                    entry.address = resources.buffer_address(source.buffer_handle);
                }
                continue;
            }
            if (!resource.lifetime.live())
            {
                continue; // culled with its passes
            }

            const view_needs& wanted = needs[index];
            if (source.kind == resource_kind::texture)
            {
                auto texture = pool.acquire_texture(context_index, resource.texture, resource.offset);
                if (!texture)
                {
                    return std::unexpected(std::move(texture.error()));
                }
                entry.physical_texture = *texture;
                if (wanted.srv)
                {
                    auto view = frames.create_frame_srv(frame, *texture);
                    if (!view)
                    {
                        return std::unexpected(std::move(view.error()));
                    }
                    entry.srv = *view;
                }
                if (wanted.uav_mips != 0)
                {
                    const std::uint32_t mips = std::min<std::uint32_t>(resource.mip_count, 64);
                    const std::span<rhi::bindless_index> uavs = arena.allocate_array<rhi::bindless_index>(mips);
                    for (std::uint32_t mip = 0; mip < mips; ++mip)
                    {
                        uavs[mip] = {};
                        if ((wanted.uav_mips & (std::uint64_t{ 1 } << mip)) == 0)
                        {
                            continue;
                        }
                        auto view = frames.create_frame_uav(frame, *texture, mip);
                        if (!view)
                        {
                            return std::unexpected(std::move(view.error()));
                        }
                        uavs[mip] = *view;
                    }
                    entry.texture_uavs = uavs;
                }
            }
            else
            {
                auto buffer = pool.acquire_buffer(context_index, resource.buffer, resource.offset);
                if (!buffer)
                {
                    return std::unexpected(std::move(buffer.error()));
                }
                entry.physical_buffer = *buffer;
                entry.address = resources.buffer_address(*buffer);
                if (wanted.srv)
                {
                    auto view = frames.create_frame_srv(frame, *buffer);
                    if (!view)
                    {
                        return std::unexpected(std::move(view.error()));
                    }
                    entry.srv = *view;
                }
                if (wanted.uav)
                {
                    auto view = frames.create_frame_uav(frame, *buffer);
                    if (!view)
                    {
                        return std::unexpected(std::move(view.error()));
                    }
                    entry.buffer_uav = *view;
                }
            }
        }
        return &resource_bag_access::create(arena, resources, entries);
    }

    // ---- the executor ----

    void record_group_function::operator()(const std::size_t group) const noexcept
    {
        executor->record(group);
    }

    void record_groups_receiver::set_value() noexcept
    {
        executor->finish();
    }

    template<class Error>
    void record_groups_receiver::set_error(Error&& error) noexcept
    {
        executor->fail(threading::detail::to_internal_error(std::forward<Error>(error)));
        executor->finish();
    }

    void record_groups_receiver::set_stopped() noexcept
    {
        executor->scheduler_stopped();
        executor->finish();
    }

    render_graph_executor::~render_graph_executor() = default;

    template<>
    std::vector<std::optional<rhi::graphics_command_list>>&
        render_graph_executor::slots<rhi::queue_type::graphics>() noexcept
    {
        return graphics_slots_;
    }

    template<>
    std::vector<std::optional<rhi::compute_command_list>>&
        render_graph_executor::slots<rhi::queue_type::compute>() noexcept
    {
        return compute_slots_;
    }

    template<>
    std::vector<std::optional<rhi::copy_command_list>>& render_graph_executor::slots<rhi::queue_type::copy>() noexcept
    {
        return copy_slots_;
    }

    template<>
    std::vector<rhi::graphics_command_list>& render_graph_executor::submit_buffer<rhi::queue_type::graphics>() noexcept
    {
        return graphics_submit_;
    }

    template<>
    std::vector<rhi::compute_command_list>& render_graph_executor::submit_buffer<rhi::queue_type::compute>() noexcept
    {
        return compute_submit_;
    }

    template<>
    std::vector<rhi::copy_command_list>& render_graph_executor::submit_buffer<rhi::queue_type::copy>() noexcept
    {
        return copy_submit_;
    }

    template<>
    std::optional<rhi::graphics_command_list>& render_graph_executor::trailing<rhi::queue_type::graphics>() noexcept
    {
        return graphics_trailing_;
    }

    template<>
    std::optional<rhi::compute_command_list>& render_graph_executor::trailing<rhi::queue_type::compute>() noexcept
    {
        return compute_trailing_;
    }

    template<>
    std::optional<rhi::copy_command_list>& render_graph_executor::trailing<rhi::queue_type::copy>() noexcept
    {
        return copy_trailing_;
    }

    template<>
    rhi::queue<rhi::queue_type::graphics> render_graph_executor::queue<rhi::queue_type::graphics>() const noexcept
    {
        return graph_.graphics_queue_;
    }

    template<>
    rhi::queue<rhi::queue_type::compute> render_graph_executor::queue<rhi::queue_type::compute>() const noexcept
    {
        return graph_.compute_queue_;
    }

    template<>
    rhi::queue<rhi::queue_type::copy> render_graph_executor::queue<rhi::queue_type::copy>() const noexcept
    {
        return graph_.copy_queue_;
    }

    void render_graph_executor::run(render_graph_execute_operation& operation, const frame_token& frame) noexcept
    {
        operation_ = &operation;
        frame_ = &frame;
        bag_ = nullptr;
        bulk_error_.reset();
        bulk_stopped_ = false;
        bulk_.reset(); // the previous frame's, completed

        try
        {
            if (!AVIS_CHECK("graphics", graph_.compiled_ != nullptr, "execute: compile the graph first"))
            {
                complete(std::unexpected(make_error(error_kind::invalid_argument, "render_graph: not compiled")));
                return;
            }
            auto bag = graph_.materialise(frame);
            if (!bag)
            {
                complete(std::unexpected(std::move(bag.error())));
                return;
            }
            bag_ = *bag;
            entries_ = resource_bag_access::entries(*bag_);

            const compiled_graph& compiled = *graph_.compiled_;
            linear_arena& arena = graph_.arena_;
            pass_barriers_ = make_array<barrier_lists>(arena, compiled.pass_barriers.size());
            for (std::size_t pass = 0; pass < compiled.pass_barriers.size(); ++pass)
            {
                pass_barriers_[pass] = convert(compiled.pass_barriers[pass]);
            }
            batch_end_barriers_ = make_array<barrier_lists>(arena, compiled.batches.size());
            for (std::size_t batch = 0; batch < compiled.batches.size(); ++batch)
            {
                batch_end_barriers_[batch] = convert(compiled.batches[batch].end_barriers);
            }
            for (std::size_t queue = 0; queue < trailing_barriers_.size(); ++queue)
            {
                trailing_barriers_[queue] = convert(compiled.end_of_frame[queue].barriers);
            }

            build_groups();

            // The trailing lists are recorded here, so a failure to acquire one stops the frame before recording
            for (auto prepared : { prepare_trailing_list<rhi::queue_type::graphics>(),
                                   prepare_trailing_list<rhi::queue_type::compute>(),
                                   prepare_trailing_list<rhi::queue_type::copy>() })
            {
                if (!prepared)
                {
                    discard();
                    complete(std::unexpected(std::move(prepared.error())));
                    return;
                }
            }
        }
        catch (...)
        {
            discard();
            complete(std::unexpected(avis::make_error(std::current_exception())));
            return;
        }

        if (groups_.empty())
        {
            finish();
            return;
        }
        try
        {
            struct connector
            {
                render_graph_executor* self;

                operator record_groups_operation() const
                {
                    return stdexec::connect(
                        stdexec::bulk(
                            stdexec::schedule(stdexec::get_parallel_scheduler()),
                            stdexec::par,
                            self->groups_.size(),
                            record_group_function{ self }),
                        record_groups_receiver{ self });
                }
            };
            bulk_.emplace(connector{ this });
        }
        catch (...)
        {
            discard();
            complete(std::unexpected(avis::make_error(std::current_exception())));
            return;
        }
        stdexec::start(*bulk_);
    }

    render_graph_executor::barrier_lists render_graph_executor::convert(const barrier_batch& batch)
    {
        linear_arena& arena = graph_.arena_;
        const std::span<rhi::texture_barrier> textures = make_array<rhi::texture_barrier>(arena, batch.textures.size());
        std::size_t texture_count = 0;
        for (const texture_transition& transition : batch.textures)
        {
            const rhi::texture_handle handle = entries_[transition.resource].physical_texture;
            if (handle.is_valid()) // an import without a handle: nothing to transition
            {
                textures[texture_count++] = { .texture = handle,
                                              .range = transition.range,
                                              .before = transition.before,
                                              .after = transition.after,
                                              .discard = transition.discard };
            }
        }
        const std::span<rhi::buffer_barrier> buffers = make_array<rhi::buffer_barrier>(arena, batch.buffers.size());
        std::size_t buffer_count = 0;
        for (const buffer_transition& transition : batch.buffers)
        {
            const rhi::buffer_handle handle = entries_[transition.resource].physical_buffer;
            if (handle.is_valid())
            {
                buffers[buffer_count++] = { .buffer = handle, .before = transition.before, .after = transition.after };
            }
        }
        return { textures.first(texture_count), buffers.first(buffer_count) };
    }

    // Groups of about group_target_milliseconds of the passes' GPU times in frame N − F, or of
    // passes_per_group_without_timings passes; never across batches
    void render_graph_executor::build_groups()
    {
        const compiled_graph& compiled = *graph_.compiled_;
        const render_graph_configuration& configuration = graph_.configuration_;
        const std::uint32_t passes_per_group =
            std::max<std::uint32_t>(configuration.passes_per_group_without_timings, 1);
        const double target = configuration.group_target_milliseconds;

        timings_.clear();
        if (graph_.profiler_ != nullptr)
        {
            for (const gpu_timing& timing : graph_.profiler_->completed_gpu_timings(*frame_))
            {
                if (timing.depth == 0)
                {
                    timings_.insert_or_assign(timing.name, timing.duration_milliseconds);
                }
            }
        }

        // The debug information shows them; it is the graph's own arena memory
        graph_debug_information& debug = *graph_.debug_information_;
        const auto debug_passes =
            std::span{ const_cast<pass_debug_information*>(debug.passes.data()), debug.passes.size() };
        for (pass_debug_information& pass : debug_passes)
        {
            const auto found = timings_.find(pass.name);
            pass.gpu_milliseconds =
                found != timings_.end() ? std::optional<double>{ found->second } : std::optional<double>{};
        }

        groups_.clear();
        std::array<std::uint32_t, 3> slot_counts{};
        const bool timed = !timings_.empty() && target > 0.0;
        for (std::uint32_t batch = 0; batch < compiled.batches.size(); ++batch)
        {
            const submission_batch& entry = compiled.batches[batch];
            std::size_t first = 0;
            while (first < entry.passes.size())
            {
                std::size_t end = first;
                if (timed)
                {
                    double milliseconds = 0.0;
                    while (end < entry.passes.size() && (end == first || milliseconds < target))
                    {
                        const auto found = timings_.find(graph_.tables_->passes[entry.passes[end]]->name);
                        milliseconds += found != timings_.end() ? found->second : target / passes_per_group;
                        ++end;
                    }
                }
                else
                {
                    end = std::min<std::size_t>(entry.passes.size(), first + passes_per_group);
                }
                std::uint32_t& slot = slot_counts[static_cast<std::size_t>(entry.queue)];
                groups_.push_back(
                    recording_group{ .batch = batch,
                                     .queue = entry.queue,
                                     .passes = entry.passes.subspan(first, end - first),
                                     .ends_batch = end == entry.passes.size(),
                                     .slot = slot++ });
                first = end;
            }
        }
        debug.recording_groups = static_cast<std::uint32_t>(groups_.size());

        graphics_slots_.clear();
        graphics_slots_.resize(slot_counts[0]);
        compute_slots_.clear();
        compute_slots_.resize(slot_counts[1]);
        copy_slots_.clear();
        copy_slots_.resize(slot_counts[2]);
        batch_points_.assign(compiled.batches.size(), rhi::synchronization_point{});
    }

    template<rhi::queue_type Queue>
    result<void> render_graph_executor::prepare_trailing_list()
    {
        trailing<Queue>().reset();
        const barrier_lists& barriers = trailing_barriers_[static_cast<std::size_t>(Queue)];
        if (barriers.empty())
        {
            return {};
        }
        auto list = graph_.command_lists_.template acquire<Queue>("render graph: end of frame");
        if (!list)
        {
            return std::unexpected(std::move(list.error()));
        }
        list->barrier({}, barriers.buffers, barriers.textures);
        trailing<Queue>().emplace(std::move(*list));
        return {};
    }

    void render_graph_executor::record(const std::size_t group) noexcept
    {
        recording_group& entry = groups_[group];
        try
        {
            switch (entry.queue)
            {
            case rhi::queue_type::graphics:
                record_group<rhi::queue_type::graphics>(entry);
                break;
            case rhi::queue_type::compute:
                record_group<rhi::queue_type::compute>(entry);
                break;
            case rhi::queue_type::copy:
                record_group<rhi::queue_type::copy>(entry);
                break;
            }
        }
        catch (...)
        {
            // An execute callback threw, for example transient_memory_exhausted from allocate_constants. The list was
            // discarded on the way out.
            entry.error = avis::make_error(std::current_exception());
        }
    }

    template<rhi::queue_type Queue>
    void render_graph_executor::record_group(recording_group& group)
    {
        auto list = graph_.command_lists_.template acquire<Queue>("render graph");
        if (!list)
        {
            group.error = std::move(list.error());
            return;
        }
        for (const std::uint32_t pass : group.passes)
        {
            record_pass<Queue>(*list, pass);
        }
        if (group.ends_batch)
        {
            // Handoffs to other queues, before the batch's signal
            const barrier_lists& barriers = batch_end_barriers_[group.batch];
            if (!barriers.empty())
            {
                list->barrier({}, barriers.buffers, barriers.textures);
            }
        }
        slots<Queue>()[group.slot].emplace(std::move(*list));
    }

    template<rhi::queue_type Queue>
    void render_graph_executor::record_pass(rhi::command_list<Queue>& list, const std::uint32_t pass)
    {
        const render_pass_node& node = *graph_.tables_->passes[pass];
        const current_pass_scope scope{ &node };

        const barrier_lists& barriers = pass_barriers_[pass];
        if (!barriers.empty())
        {
            list.barrier({}, barriers.buffers, barriers.textures);
        }
        list.begin_event(node.name);
        gpu_profiler* const profiler = graph_.profiler_;
        gpu_scope_id timing;
        if (profiler != nullptr)
        {
            timing = profiler->begin_timing(*frame_, list, node.name);
        }

        frame_scheduler* const frames = &graph_.frames_;
        const auto call = [&]<rhi::queue_type Recorder>(rhi::command_recorder<Recorder>& recorder, auto trampoline)
        {
            pass_context<Recorder> context{ recorder, frame_, frames, node.render_width, node.render_height };
            trampoline(node.callable, node.data, *bag_, context);
        };
        switch (node.type)
        {
        case pass_type::graphics:
            if constexpr (Queue == rhi::queue_type::graphics)
            {
                call(list, node.execute_graphics);
            }
            break;
        case pass_type::compute:
            if constexpr (Queue == rhi::queue_type::graphics)
            {
                rhi::compute_recorder recorder = list.template narrow<rhi::queue_type::compute>();
                call(recorder, node.execute_compute);
            }
            else if constexpr (Queue == rhi::queue_type::compute)
            {
                call(list, node.execute_compute);
            }
            break;
        case pass_type::copy:
            if constexpr (Queue == rhi::queue_type::copy)
            {
                call(list, node.execute_copy);
            }
            else
            {
                rhi::copy_recorder recorder = list.template narrow<rhi::queue_type::copy>();
                call(recorder, node.execute_copy);
            }
            break;
        }

        if (profiler != nullptr)
        {
            profiler->end_timing(*frame_, list, timing);
        }
        list.end_event();
    }

    void render_graph_executor::fail(avis::error error) noexcept
    {
        if (!bulk_error_)
        {
            bulk_error_ = std::move(error);
        }
    }

    void render_graph_executor::scheduler_stopped() noexcept
    {
        bulk_stopped_ = true;
    }

    // After every group: the receiver's stop request first, then the first failure; otherwise the batches in order,
    // then the trailing lists
    void render_graph_executor::finish() noexcept
    {
        if (operation_->stop_requested() || bulk_stopped_)
        {
            discard();
            render_graph_execute_operation* const operation = operation_;
            operation_ = nullptr;
            operation->complete_stopped();
            return;
        }
        if (bulk_error_)
        {
            discard();
            complete(std::unexpected(std::move(*bulk_error_)));
            return;
        }
        for (recording_group& group : groups_)
        {
            if (group.error)
            {
                avis::error error = std::move(*group.error);
                discard();
                complete(std::unexpected(std::move(error)));
                return;
            }
        }

        try
        {
            const compiled_graph& compiled = *graph_.compiled_;
            std::array<rhi::synchronization_point, 3> last{};
            std::size_t first_group = 0;
            for (std::uint32_t batch = 0; batch < compiled.batches.size(); ++batch)
            {
                const rhi::queue_type queue = compiled.batches[batch].queue;
                result<rhi::synchronization_point> point = rhi::synchronization_point{};
                switch (queue)
                {
                case rhi::queue_type::graphics:
                    point = submit_batch<rhi::queue_type::graphics>(batch, first_group);
                    break;
                case rhi::queue_type::compute:
                    point = submit_batch<rhi::queue_type::compute>(batch, first_group);
                    break;
                case rhi::queue_type::copy:
                    point = submit_batch<rhi::queue_type::copy>(batch, first_group);
                    break;
                }
                if (!point)
                {
                    discard();
                    complete(std::unexpected(std::move(point.error())));
                    return;
                }
                batch_points_[batch] = *point;
                last[static_cast<std::size_t>(queue)] = *point;
                while (first_group < groups_.size() && groups_[first_group].batch == batch)
                {
                    ++first_group;
                }
            }

            const auto trailing_point = [&]<rhi::queue_type Queue>() -> result<void>
            {
                auto point = submit_trailing<Queue>();
                if (!point)
                {
                    return std::unexpected(std::move(point.error()));
                }
                if (point->has_value())
                {
                    last[static_cast<std::size_t>(Queue)] = **point;
                }
                return {};
            };
            for (auto submitted : { trailing_point.template operator()<rhi::queue_type::graphics>(),
                                    trailing_point.template operator()<rhi::queue_type::compute>(),
                                    trailing_point.template operator()<rhi::queue_type::copy>() })
            {
                if (!submitted)
                {
                    discard();
                    complete(std::unexpected(std::move(submitted.error())));
                    return;
                }
            }

            submitted_frame frame;
            for (const rhi::synchronization_point point : last)
            {
                if (point.value != 0)
                {
                    frame.points[frame.count++] = point;
                }
            }
            complete(frame);
        }
        catch (...)
        {
            discard();
            complete(std::unexpected(avis::make_error(std::current_exception())));
        }
    }

    template<rhi::queue_type Queue>
    result<rhi::synchronization_point>
        render_graph_executor::submit_batch(const std::uint32_t batch, const std::size_t first_group)
    {
        const submission_batch& entry = graph_.compiled_->batches[batch];
        waits_.clear();
        for (const std::uint32_t waited : entry.waits)
        {
            waits_.push_back(batch_points_[waited]);
        }
        // The RHI skips points that are already complete
        waits_.insert(waits_.end(), entry.ready_after.begin(), entry.ready_after.end());

        std::vector<rhi::command_list<Queue>>& lists = submit_buffer<Queue>();
        lists.clear();
        for (std::size_t group = first_group; group < groups_.size() && groups_[group].batch == batch; ++group)
        {
            lists.push_back(std::move(*slots<Queue>()[groups_[group].slot]));
            slots<Queue>()[groups_[group].slot].reset();
        }
        auto point = queue<Queue>().submit(lists, waits_);
        lists.clear();
        return point;
    }

    template<rhi::queue_type Queue>
    result<std::optional<rhi::synchronization_point>> render_graph_executor::submit_trailing()
    {
        if (!trailing<Queue>())
        {
            return std::nullopt;
        }
        waits_.clear();
        for (const std::uint32_t waited : graph_.compiled_->end_of_frame[static_cast<std::size_t>(Queue)].waits)
        {
            if (waited != no_index)
            {
                waits_.push_back(batch_points_[waited]);
            }
        }
        std::vector<rhi::command_list<Queue>>& lists = submit_buffer<Queue>();
        lists.clear();
        lists.push_back(std::move(*trailing<Queue>()));
        trailing<Queue>().reset();
        auto point = queue<Queue>().submit(lists, waits_);
        lists.clear();
        if (!point)
        {
            return std::unexpected(std::move(point.error()));
        }
        return *point;
    }

    void render_graph_executor::complete(result<submitted_frame> outcome) noexcept
    {
        render_graph_execute_operation* const operation = operation_;
        operation_ = nullptr;
        operation->complete(std::move(outcome));
    }

    // Unsubmitted lists are discarded: closed, their allocators reused at once
    void render_graph_executor::discard() noexcept
    {
        graphics_slots_.clear();
        compute_slots_.clear();
        copy_slots_.clear();
        graphics_submit_.clear();
        compute_submit_.clear();
        copy_submit_.clear();
        graphics_trailing_.reset();
        compute_trailing_.reset();
        copy_trailing_.reset();
    }

    void render_graph_execute_operation::begin() noexcept
    {
        graph_->executor_->run(*this, *frame_);
    }
} // namespace avis::graphics::detail
