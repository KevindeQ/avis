#include "rendering/range_walker.h"

#include "avis/detail/check.h"
#include "avis/graphics/binding_layout.h"
#include "avis/graphics/linear_arena.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <shared_types.hlsli>
#include <string_view>
#include <variant>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "renderer";

        // An argument offset's minimum size: every command item gets its own offset, so values are unique
        constexpr std::size_t minimum_argument_size = 4;

        bool same_mesh(const draw_record& a, const draw_record& b) noexcept
        {
            return a.mesh == b.mesh && a.submesh == b.submesh;
        }

        std::uint32_t shader_attributes(const avis::flags<scene::vertex_attribute> attributes) noexcept
        {
            std::uint32_t mask = gpu::vertex_attribute_position;
            if (attributes & scene::vertex_attribute::normal)
            {
                mask |= gpu::vertex_attribute_normal;
            }
            if (attributes & scene::vertex_attribute::tangent)
            {
                mask |= gpu::vertex_attribute_tangent;
            }
            if (attributes & scene::vertex_attribute::uv0)
            {
                mask |= gpu::vertex_attribute_uv0;
            }
            if (attributes & scene::vertex_attribute::color0)
            {
                mask |= gpu::vertex_attribute_color0;
            }
            return mask;
        }

        std::uint32_t stream_offset(const std::uint32_t offset) noexcept
        {
            return offset == vertex_stream_offsets::absent_stream ? gpu::vertex_stream_absent : offset;
        }

        bool same_constants(const gpu::scene_draw_constants& a, const gpu::scene_draw_constants& b) noexcept
        {
            return std::memcmp(&a, &b, sizeof(a)) == 0;
        }

        bool same_viewport(const rhi::viewport& a, const rhi::viewport& b) noexcept
        {
            return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height &&
                   a.minimum_depth == b.minimum_depth && a.maximum_depth == b.maximum_depth;
        }

        bool same_rectangle(const rhi::rectangle& a, const rhi::rectangle& b) noexcept
        {
            return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
        }

        std::string_view name_of(const fullscreen_layer layer) noexcept
        {
            switch (layer)
            {
            case fullscreen_layer::scene:
                return "scene";
            case fullscreen_layer::fullscreen_effects:
                return "fullscreen_effects";
            case fullscreen_layer::hud:
                return "hud";
            }
            return "reserved";
        }

        std::string_view name_of(const viewport_layer layer) noexcept
        {
            switch (layer)
            {
            case viewport_layer::background:
                return "background";
            case viewport_layer::world:
                return "world";
            case viewport_layer::effects:
                return "effects";
            case viewport_layer::overlay:
                return "overlay";
            case viewport_layer::overlay_on_top:
                return "overlay_on_top";
            }
            return "reserved";
        }

        std::string_view name_of(const translucency value) noexcept
        {
            switch (value)
            {
            case translucency::opaque:
                return "opaque";
            case translucency::blended:
                return "blended";
            case translucency::additive:
                return "additive";
            case translucency::subtractive:
                return "subtractive";
            }
            return "reserved";
        }

        // The walk of one part: its filter and its current group and viewport
        class walker
        {
        public:
            walker(
                const pass_kind kind,
                graphics::graphics_pass_context& context,
                const range_walk_sources& sources,
                const pass_bindings& bindings) noexcept :
                kind_{ kind },
                context_{ context },
                recorder_{ context.recorder() },
                sources_{ sources },
                bindings_{ bindings },
                items_{ sources.list.list->items() },
                records_{ sources.list.list->records() }
            {}

            range_walk_statistics walk(const std::span<const draw_run> runs)
            {
                for (const draw_run& run : runs)
                {
                    ++statistics_.runs;
                    const draw_item& first = items_[run.first];
                    const key_group group = group_of(first.key);
                    enter_group(group);
                    const viewport_binding* const binding = enter_viewport(group.viewport);
                    if (binding == nullptr)
                    {
                        statistics_.skipped_invalid += run.count;
                        continue;
                    }
                    if (is_command_key(first.key))
                    {
                        execute_command(first, group, *binding);
                    }
                    else
                    {
                        draw(run, first, *binding);
                    }
                }
                if (open_event_)
                {
                    recorder_.end_event();
                }
                return statistics_;
            }

        private:
            void enter_group(const key_group& group)
            {
                if (open_event_ && *open_event_ == group)
                {
                    return;
                }
                if (open_event_)
                {
                    recorder_.end_event();
                }
                recorder_.begin_event(group_name(group));
                open_event_ = group;
                ++statistics_.events;
            }

            // The binding of `point`, binding its targets when the viewport changes; nullptr when the pass has none
            const viewport_binding* enter_viewport(const viewport point)
            {
                const std::optional<viewport_binding>& binding = bindings_.viewports[point.index];
                if (!AVIS_CHECK(
                        logger_name,
                        binding.has_value(),
                        std::format(
                            "range walker ({}): an item of viewport {} that the pass doesn't bind isn't recorded",
                            to_string(kind_),
                            point.index)))
                {
                    return nullptr;
                }
                if (current_viewport_ != point.index)
                {
                    current_viewport_ = point.index;
                    recorder_.set_render_targets(binding->render_targets, binding->depth);
                    ++statistics_.viewport_changes;
                }
                return &*binding;
            }

            // The viewport and scissor (overridden or the binding's), the pipeline with its root arguments, and the
            // draw constants, each only when it changed
            void apply_state(const viewport_binding& binding, const pipeline_slot& slot)
            {
                const rhi::viewport& viewport = viewport_override_ ? viewport_override_->viewport : binding.viewport;
                const rhi::rectangle& scissor = viewport_override_ ? viewport_override_->scissor : binding.scissor;
                if (!viewport_ || !same_viewport(*viewport_, viewport))
                {
                    recorder_.set_viewport(viewport);
                    viewport_ = viewport;
                    ++statistics_.viewport_sets;
                }
                if (!scissor_ || !same_rectangle(*scissor_, scissor))
                {
                    recorder_.set_scissor(scissor);
                    scissor_ = scissor;
                    ++statistics_.scissor_sets;
                }

                if (slot.pipeline != pipeline_)
                {
                    recorder_.set_pipeline(slot.pipeline);
                    pipeline_ = slot.pipeline;
                    ++statistics_.pipeline_changes;
                    if (slot.root_signature != signature_)
                    {
                        // The root arguments are undefined after a signature switch (18 §10.3)
                        signature_ = slot.root_signature;
                        ++statistics_.signature_changes;
                        pass_constants_.reset();
                        view_constants_.reset();
                        constants_.reset();
                    }
                }
                if (!pass_constants_ && bindings_.pass_constants.value != 0)
                {
                    graphics::binding_layout::set_pass_constants(recorder_, bindings_.pass_constants);
                    pass_constants_ = bindings_.pass_constants;
                }
                if (binding.view_constants.value != 0 && view_constants_ != binding.view_constants)
                {
                    graphics::binding_layout::set_view_constants(recorder_, binding.view_constants);
                    view_constants_ = binding.view_constants;
                }
            }

            // The set's PSO for the pass, through the write-mask override
            pipeline_slot slot_of(const std::uint32_t set)
            {
                if (write_mask_ == write_mask::all)
                {
                    return sources_.pipelines.set(set).slot(kind_);
                }
                auto variant = sources_.pipelines.write_mask_variant(set, kind_, write_mask_);
                if (!AVIS_CHECK(
                        logger_name,
                        variant.has_value(),
                        std::format(
                            "range walker ({}): no write-mask variant {:#x} of set {}, drawn without the mask: {}",
                            to_string(kind_),
                            write_mask_,
                            set,
                            variant ? std::string_view{} : std::string_view{ variant.error().message })))
                {
                    return sources_.pipelines.set(set).slot(kind_);
                }
                return *variant;
            }

            void draw(const draw_run& run, const draw_item& first, const viewport_binding& binding)
            {
                const draw_record& record = records_[first.value];
                const std::uint32_t set = static_cast<std::uint32_t>(key_fields::pipeline_set.extract(first.key));
                const pipeline_slot slot = slot_of(set);
                if (!slot.drawn())
                {
                    statistics_.skipped_not_drawn += run.count;
                    return;
                }
                const bool mesh_found = record.mesh < sources_.meshes.size() &&
                                        record.submesh < sources_.meshes[record.mesh].primitives.size();
                if (!AVIS_CHECK(
                        logger_name,
                        mesh_found,
                        std::format(
                            "range walker: record {} has mesh {} submesh {}, which the mesh table doesn't have",
                            first.value,
                            record.mesh,
                            record.submesh)))
                {
                    statistics_.skipped_invalid += run.count;
                    return;
                }
                const draw_mesh& mesh = sources_.meshes[record.mesh];
                const scene::mesh_primitive& primitive = mesh.primitives[record.submesh];
                if (primitive.vertex_count == 0)
                {
                    return;
                }
                // A progressive run draws a prefix of the vertices (11 §9.3); indexed lines and triangles draw whole
                const bool prefix = run.vertex_count > 0 &&
                                    (mesh.topology == scene::mesh_topology::points || primitive.index_count == 0);
                const std::uint32_t vertex_count =
                    prefix ? std::min(run.vertex_count, primitive.vertex_count) : primitive.vertex_count;

                apply_state(binding, slot);
                if (!index_buffer_bound_ && primitive.index_count > 0 &&
                    mesh.topology == scene::mesh_topology::triangles)
                {
                    // Once per walk (each recording group), unless a callback intervened
                    recorder_.set_index_buffer(sources_.index_buffer, rhi::index_type::uint32);
                    index_buffer_bound_ = true;
                }
                const geometry_range& range = mesh.range;
                const gpu::scene_draw_constants constants{
                    .instance_index_offset = run.instance_offset,
                    .material = static_cast<std::uint32_t>(key_fields::material.extract(first.key)),
                    .mesh = record.mesh,
                    .attributes = shader_attributes(mesh.attributes),
                    .vertex_first = primitive.vertex_first,
                    .vertex_count = vertex_count,
                    .index_first = range.first_index() + primitive.index_first,
                    .index_count = primitive.index_count,
                    .position_offset = stream_offset(range.streams.position),
                    .normal_offset = stream_offset(range.streams.normal),
                    .tangent_offset = stream_offset(range.streams.tangent),
                    .uv0_offset = stream_offset(range.streams.uv0),
                    .color0_offset = stream_offset(range.streams.color0),
                    .padding0 = 0,
                    .padding1 = 0,
                    .padding2 = 0,
                };
                if (!constants_ || !same_constants(*constants_, constants))
                {
                    graphics::binding_layout::push_draw_constants(recorder_, constants);
                    constants_ = constants;
                    ++statistics_.constant_pushes;
                }

                // Lines and points are expanded into quads in the vertex shader, six vertices each (11 §9)
                const std::uint32_t instances = run.count;
                switch (mesh.topology)
                {
                case scene::mesh_topology::triangles:
                    if (primitive.index_count > 0)
                    {
                        recorder_.draw_indexed(primitive.index_count, instances, constants.index_first, 0, 0);
                    }
                    else
                    {
                        const std::uint32_t triangle_vertices = vertex_count / 3 * 3;
                        if (triangle_vertices == 0)
                        {
                            return;
                        }
                        recorder_.draw(triangle_vertices, instances, 0, 0);
                    }
                    break;
                case scene::mesh_topology::lines:
                    {
                        const std::uint32_t segments =
                            (primitive.index_count > 0 ? primitive.index_count : vertex_count) / 2;
                        if (segments == 0)
                        {
                            return;
                        }
                        recorder_.draw(segments * 6, instances, 0, 0);
                        break;
                    }
                case scene::mesh_topology::points:
                    recorder_.draw(vertex_count * 6, instances, 0, 0);
                    break;
                }
                ++statistics_.draws;
                statistics_.instances += instances;
            }

            // The arguments of a command item as a T, or nullopt (a failed debug check) when they don't fit
            template<typename T>
            std::optional<T> arguments_of(const draw_item& item) const
            {
                const std::span<const std::byte> arguments = sources_.list.list->command_arguments();
                if (!AVIS_CHECK(
                        logger_name,
                        item.value <= arguments.size() && arguments.size() - item.value >= sizeof(T),
                        std::format(
                            "range walker: command arguments at {} need {} bytes, the list has {}",
                            item.value,
                            sizeof(T),
                            arguments.size())))
                {
                    return std::nullopt;
                }
                T value;
                std::memcpy(&value, arguments.data() + item.value, sizeof(T));
                return value;
            }

            void execute_command(const draw_item& item, const key_group& group, const viewport_binding& binding)
            {
                ++statistics_.commands;
                const command_key_fields fields = std::get<command_key_fields>(decode_key(item.key));
                if (const auto* const callback = std::get_if<callback_command>(&fields.command))
                {
                    const std::span<const std::byte> arguments = sources_.list.list->command_arguments();
                    const void* const data = item.value < arguments.size() ? arguments.data() + item.value : nullptr;
                    command_context context{ context_, binding, group };
                    callback->function()(context, data);
                    forget();
                    return;
                }

                const builtin_command& builtin = std::get<builtin_command>(fields.command);
                switch (builtin.opcode)
                {
                case command_opcode::clear_depth_stencil:
                    if (const auto arguments = arguments_of<clear_depth_stencil_arguments>(item))
                    {
                        clear_depth_stencil(*arguments, binding);
                    }
                    else
                    {
                        ++statistics_.skipped_invalid;
                    }
                    break;
                case command_opcode::clear_render_target:
                    if (const auto arguments = arguments_of<clear_render_target_arguments>(item))
                    {
                        clear_render_target(*arguments, binding);
                    }
                    else
                    {
                        ++statistics_.skipped_invalid;
                    }
                    break;
                case command_opcode::set_viewport_scissor:
                    if (const auto arguments = arguments_of<viewport_scissor_arguments>(item))
                    {
                        viewport_override_ = *arguments;
                    }
                    else
                    {
                        ++statistics_.skipped_invalid;
                    }
                    break;
                case command_opcode::set_stencil_reference:
                    // The selection mask is an R8 target, not a stencil bit (the task's Open point), so the depth
                    // formats have no stencil and the RHI has no set_stencil_reference
                    AVIS_CHECK(
                        logger_name,
                        false,
                        "range walker: set_stencil_reference isn't supported (no stencil in iteration 1)");
                    ++statistics_.skipped_invalid;
                    break;
                case command_opcode::set_write_mask:
                    write_mask_ = static_cast<std::uint8_t>(builtin.immediate & write_mask::all);
                    break;
                case command_opcode::reset_state:
                    viewport_override_.reset();
                    write_mask_ = write_mask::all;
                    break;
                default:
                    AVIS_CHECK(
                        logger_name,
                        false,
                        std::format(
                            "range walker: unknown command opcode {}", static_cast<std::uint32_t>(builtin.opcode)));
                    ++statistics_.skipped_invalid;
                    break;
                }
            }

            void clear_depth_stencil(const clear_depth_stencil_arguments& arguments, const viewport_binding& binding)
            {
                if (!AVIS_CHECK(
                        logger_name,
                        bindings_.depth_written && binding.depth.has_value(),
                        std::format(
                            "range walker ({}): clear_depth_stencil on a depth attachment the pass doesn't write",
                            to_string(kind_))))
                {
                    ++statistics_.skipped_invalid;
                    return;
                }
                AVIS_CHECK(
                    logger_name,
                    (arguments.flags & clear_flags::stencil) == 0,
                    "range walker: clear_depth_stencil can't clear stencil (no stencil in iteration 1)");
                if ((arguments.flags & clear_flags::depth) != 0)
                {
                    recorder_.clear_depth(*binding.depth, arguments.depth);
                }
            }

            void clear_render_target(const clear_render_target_arguments& arguments, const viewport_binding& binding)
            {
                if (!AVIS_CHECK(
                        logger_name,
                        arguments.attachment < binding.render_targets.size(),
                        std::format(
                            "range walker ({}): clear_render_target on attachment {}, the pass writes {}",
                            to_string(kind_),
                            arguments.attachment,
                            binding.render_targets.size())))
                {
                    ++statistics_.skipped_invalid;
                    return;
                }
                recorder_.clear_render_target(binding.render_targets[arguments.attachment], arguments.color);
            }

            // After a callback: everything may have changed behind the filter
            void forget() noexcept
            {
                pipeline_ = {};
                signature_ = {};
                pass_constants_.reset();
                view_constants_.reset();
                constants_.reset();
                viewport_.reset();
                scissor_.reset();
                current_viewport_ = no_viewport;
                index_buffer_bound_ = false;
            }

            static constexpr std::uint32_t no_viewport = std::numeric_limits<std::uint32_t>::max();

            pass_kind kind_;
            graphics::graphics_pass_context& context_;
            rhi::graphics_recorder& recorder_;
            const range_walk_sources& sources_;
            const pass_bindings& bindings_;
            std::span<const draw_item> items_;
            std::span<const draw_record> records_;
            range_walk_statistics statistics_;

            std::optional<key_group> open_event_;
            std::uint32_t current_viewport_ = no_viewport;
            bool index_buffer_bound_ = false;

            // The redundant-state filter
            rhi::graphics_pipeline_handle pipeline_;
            rhi::root_signature_handle signature_;
            std::optional<rhi::gpu_address> pass_constants_;
            std::optional<rhi::gpu_address> view_constants_;
            std::optional<gpu::scene_draw_constants> constants_;
            std::optional<rhi::viewport> viewport_;
            std::optional<rhi::rectangle> scissor_;

            // Command overrides
            std::optional<viewport_scissor_arguments> viewport_override_;
            std::uint8_t write_mask_ = write_mask::all;
        };
    } // namespace

    std::span<const draw_run> prepared_draw_list::runs_of(const std::span<const draw_item> items) const noexcept
    {
        if (list == nullptr || items.empty())
        {
            return {};
        }
        const std::span<const draw_item> all = list->items();
        const auto first = static_cast<std::uint32_t>(items.data() - all.data());
        const auto end = first + static_cast<std::uint32_t>(items.size());
        const auto begin_run = std::ranges::lower_bound(runs, first, {}, &draw_run::first);
        const auto end_run = std::ranges::lower_bound(begin_run, runs.end(), end, {}, &draw_run::first);
        return { begin_run, end_run };
    }

    prepared_draw_list prepare_draw_runs(
        const draw_list& list,
        const std::span<const draw_mesh> meshes,
        graphics::linear_arena& arena,
        std::vector<std::uint32_t>& instance_indices,
        const std::span<const progressive_vertex_counts> progressive)
    {
        const std::span<const draw_item> items = list.items();
        const std::span<const draw_record> records = list.records();
        const std::span<draw_run> runs = arena.allocate_array<draw_run>(items.size());
        std::size_t run_count = 0;
        instance_indices.reserve(instance_indices.size() + items.size());

        for (std::size_t index = 0; index < items.size();)
        {
            const draw_item& item = items[index];
            if (is_command_key(item.key))
            {
                runs[run_count++] = { .first = static_cast<std::uint32_t>(index), .count = 1, .instance_offset = 0 };
                ++index;
                continue;
            }

            // Equal keys whose records share the mesh and submesh; a progressive mesh draws each instance alone
            const draw_record& record = records[item.value];
            const bool is_progressive = record.mesh < meshes.size() && meshes[record.mesh].progressive;
            std::size_t end = index + 1;
            std::uint32_t vertex_count = 0;
            if (is_progressive)
            {
                if (item.value < progressive.size())
                {
                    const progressive_vertex_counts& counts = progressive[item.value];
                    vertex_count = key_fields::viewport.extract(item.key) == 0 ? counts.camera : counts.shadow;
                }
            }
            else
            {
                while (end < items.size() && items[end].key == item.key && same_mesh(records[items[end].value], record))
                {
                    ++end;
                }
            }
            runs[run_count++] = { .first = static_cast<std::uint32_t>(index),
                                  .count = static_cast<std::uint32_t>(end - index),
                                  .instance_offset = static_cast<std::uint32_t>(instance_indices.size()),
                                  .vertex_count = vertex_count };
            for (; index < end; ++index)
            {
                instance_indices.push_back(records[items[index].value].instance);
            }
        }
        return { .list = &list, .runs = runs.first(run_count) };
    }

    std::uint64_t progressive_vertices(const prepared_draw_list& list) noexcept
    {
        std::uint64_t total = 0;
        for (const draw_run& run : list.runs)
        {
            total += run.vertex_count;
        }
        return total;
    }

    // ---- draw_commands ----

    std::uint32_t draw_commands::append(const std::span<const std::byte> bytes)
    {
        const std::size_t offset = (arguments_.size() + command_argument_alignment - 1) / command_argument_alignment *
                                   command_argument_alignment;
        arguments_.resize(offset + std::max(bytes.size(), minimum_argument_size));
        if (!bytes.empty())
        {
            std::memcpy(arguments_.data() + offset, bytes.data(), bytes.size());
        }
        return static_cast<std::uint32_t>(offset);
    }

    void draw_commands::add(const command_key_fields& fields, const std::span<const std::byte> arguments)
    {
        items_.push_back({ .key = make_command_key(fields), .value = append(arguments) });
    }

    void draw_commands::clear_depth_stencil(
        const key_group group, const std::uint32_t sequence, const clear_depth_stencil_arguments& arguments)
    {
        add({ group, sequence, builtin_command{ command_opcode::clear_depth_stencil, 0 } },
            std::as_bytes(std::span{ &arguments, 1 }));
    }

    void draw_commands::clear_render_target(
        const key_group group, const std::uint32_t sequence, const clear_render_target_arguments& arguments)
    {
        add({ group, sequence, builtin_command{ command_opcode::clear_render_target, 0 } },
            std::as_bytes(std::span{ &arguments, 1 }));
    }

    void draw_commands::set_viewport_scissor(
        const key_group group, const std::uint32_t sequence, const viewport_scissor_arguments& arguments)
    {
        add({ group, sequence, builtin_command{ command_opcode::set_viewport_scissor, 0 } },
            std::as_bytes(std::span{ &arguments, 1 }));
    }

    void draw_commands::set_stencil_reference(
        const key_group group, const std::uint32_t sequence, const std::uint8_t reference)
    {
        add({ group, sequence, builtin_command{ command_opcode::set_stencil_reference, reference } }, {});
    }

    void draw_commands::set_write_mask(const key_group group, const std::uint32_t sequence, const std::uint8_t mask)
    {
        add({ group, sequence, builtin_command{ command_opcode::set_write_mask, mask } }, {});
    }

    void draw_commands::reset_state(const key_group group, const std::uint32_t sequence)
    {
        add({ group, sequence, builtin_command{ command_opcode::reset_state, 0 } }, {});
    }

    void draw_commands::callback(
        const key_group group,
        const std::uint32_t sequence,
        const command_callback function,
        const std::span<const std::byte> arguments)
    {
        add({ group, sequence, callback_command::of(function) }, arguments);
    }

    // ---- walking ----

    range_walk_statistics& range_walk_statistics::operator+=(const range_walk_statistics& other) noexcept
    {
        runs += other.runs;
        draws += other.draws;
        instances += other.instances;
        commands += other.commands;
        events += other.events;
        pipeline_changes += other.pipeline_changes;
        signature_changes += other.signature_changes;
        constant_pushes += other.constant_pushes;
        viewport_changes += other.viewport_changes;
        viewport_sets += other.viewport_sets;
        scissor_sets += other.scissor_sets;
        skipped_not_drawn += other.skipped_not_drawn;
        skipped_invalid += other.skipped_invalid;
        return *this;
    }

    std::string group_name(const key_group& group)
    {
        std::string viewport_name;
        if (group.viewport.is_camera())
        {
            viewport_name = "camera";
        }
        else if (group.viewport.is_shadow_cascade())
        {
            viewport_name = std::format("shadow_cascade[{}]", group.viewport.cascade());
        }
        else
        {
            viewport_name = std::format("viewport[{}]", group.viewport.index);
        }
        return std::format(
            "{}/{}/{}/{}",
            name_of(group.fullscreen_layer),
            viewport_name,
            name_of(group.viewport_layer),
            name_of(group.translucency));
    }

    range_walk_statistics walk(
        const std::span<const draw_run> runs,
        const pass_kind kind,
        graphics::graphics_pass_context& context,
        const range_walk_sources& sources,
        const pass_bindings& bindings)
    {
        if (!AVIS_CHECK(logger_name, sources.list.list != nullptr, "range walker: a prepared list without its list"))
        {
            return {};
        }
        return walker{ kind, context, sources, bindings }.walk(runs);
    }

    // ---- recording groups ----

    std::size_t item_count(const std::span<const draw_run> runs) noexcept
    {
        std::size_t count = 0;
        for (const draw_run& run : runs)
        {
            count += run.count;
        }
        return count;
    }

    std::uint32_t recording_part_count(const std::size_t items, const recording_split& split) noexcept
    {
        const std::size_t per_part = std::max<std::size_t>(split.items_per_part, 1);
        if (items <= std::size_t{ std::max(split.passes_per_group, 1u) } * per_part)
        {
            return 1;
        }
        const std::size_t parts = (items + per_part - 1) / per_part;
        return static_cast<std::uint32_t>(std::clamp<std::size_t>(parts, 1, std::max(split.maximum_parts, 1u)));
    }

    std::vector<std::span<const draw_run>> split_runs(const std::span<const draw_run> runs, const std::uint32_t parts)
    {
        std::vector<std::span<const draw_run>> result;
        if (runs.empty())
        {
            return result;
        }
        const std::size_t total = item_count(runs);
        const std::size_t part_count = std::clamp<std::size_t>(parts, 1, runs.size());
        result.reserve(part_count);

        // Each part ends at the first run boundary at or past its share of the items
        std::size_t begin = 0;
        std::size_t done = 0;
        for (std::size_t part = 1; part <= part_count && begin < runs.size(); ++part)
        {
            const std::size_t target = total * part / part_count;
            std::size_t end = begin;
            // At least one run, and enough left for the parts after this one
            while (end < runs.size() && (done < target || end == begin) &&
                   (end == begin || runs.size() - end > part_count - part))
            {
                done += runs[end].count;
                ++end;
            }
            if (part == part_count)
            {
                end = runs.size();
            }
            result.push_back(runs.subspan(begin, end - begin));
            begin = end;
        }
        return result;
    }
} // namespace avis::visualizer::rendering
