#include "rendering/passes/scene_range_pass.h"

#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"

#include <format>
#include <optional>
#include <string>
#include <vector>

namespace avis::visualizer::rendering
{
    namespace
    {
        struct scene_pass_data
        {
            std::array<graphics::texture, 8> colors{};
            std::size_t color_count = 0;
            graphics::texture depth;
            graphics::buffer instances;
            graphics::buffer instance_indices;
            graphics::buffer vertices;
            graphics::buffer indices;
            graphics::buffer materials;
        };

        // The clears the first part records: a value per attachment that asked for one
        struct attachment_clears
        {
            std::array<std::optional<std::array<float, 4>>, 8> colors{};
            std::optional<float> depth;
        };

        graphics::texture write_attachment(
            graphics::render_pass_builder& builder,
            const scene_attachment& attachment,
            const graphics::texture current,
            const bool first_part,
            const graphics::texture_write usage)
        {
            graphics::texture input = current;
            if (first_part && !input.valid())
            {
                input = builder.create_texture(attachment.name, attachment.description);
            }
            const bool clear = first_part && attachment.clear;
            return builder.write(
                input,
                usage,
                graphics::all_subresources,
                clear ? graphics::write_flags::needs_clear : graphics::write_flags::none);
        }
    } // namespace

    scene_pass_outputs add_scene_range_pass(
        graphics::render_graph& graph,
        const scene_range_pass& pass,
        const scene_pass_inputs& inputs,
        const scene_attachments& attachments)
    {
        const std::span<const draw_run> runs = inputs.list.runs_of(inputs.list.list->range(pass.range));
        const std::uint32_t part_count = recording_part_count(item_count(runs), inputs.split);
        std::vector<std::span<const draw_run>> parts = split_runs(runs, part_count);
        if (parts.empty())
        {
            parts.emplace_back(); // one pass even for an empty range: it creates and clears the attachments
        }

        scene_pass_outputs outputs{ .color_count = attachments.color_count, .parts = parts.size() };
        for (std::size_t index = 0; index < attachments.color_count; ++index)
        {
            outputs.colors[index] = attachments.colors[index].texture;
        }
        if (attachments.depth)
        {
            outputs.depth = attachments.depth->texture;
        }

        std::vector<scene_viewport> viewports{ pass.viewports.begin(), pass.viewports.end() };
        if (viewports.empty())
        {
            viewports.push_back({ .point = viewport::camera(), .layer = 0, .view_constants = inputs.view_constants });
        }

        attachment_clears clears;
        for (std::size_t index = 0; index < attachments.color_count; ++index)
        {
            const scene_attachment& attachment = attachments.colors[index];
            if (attachment.clear)
            {
                clears.colors[index] = attachment.description.clear_value.value_or(rhi::clear_value{}).color;
            }
        }
        if (attachments.depth && attachments.depth->clear)
        {
            clears.depth = attachments.depth->description.clear_value.value_or(rhi::clear_value{}).depth;
        }

        for (std::size_t part = 0; part < parts.size(); ++part)
        {
            const std::string name = part == 0 ? std::string{ pass.name } : std::format("{}_part{}", pass.name, part);
            const scene_pass_data& written = graph.add_graphics_pass<scene_pass_data>(
                name,
                [&](graphics::render_pass_builder& builder, scene_pass_data& data)
                {
                    const bool first_part = part == 0;
                    data.color_count = attachments.color_count;
                    for (std::size_t index = 0; index < attachments.color_count; ++index)
                    {
                        data.colors[index] = write_attachment(
                            builder,
                            attachments.colors[index],
                            outputs.colors[index],
                            first_part,
                            graphics::texture_write::render_target);
                    }
                    if (attachments.depth)
                    {
                        data.depth = write_attachment(
                            builder,
                            *attachments.depth,
                            outputs.depth,
                            first_part,
                            graphics::texture_write::depth_write);
                    }
                    data.instances = builder.read(inputs.frame.instances, graphics::buffer_read::srv_graphics);
                    data.instance_indices =
                        builder.read(inputs.frame.instance_indices, graphics::buffer_read::srv_graphics);
                    data.vertices = builder.read(inputs.geometry.vertices, graphics::buffer_read::srv_graphics);
                    data.indices = builder.read(inputs.geometry.indices, graphics::buffer_read::index_and_srv_graphics);
                    data.materials = builder.read(inputs.materials.table, graphics::buffer_read::srv_graphics);
                },
                [runs = parts[part],
                 kind = pass.kind,
                 line_margin = pass.line_margin,
                 list = &inputs.list,
                 meshes = inputs.meshes,
                 pipelines = &inputs.pipelines,
                 viewports,
                 clears = part == 0 ? clears : attachment_clears{},
                 instances_srv = inputs.frame.instances_srv,
                 instance_indices_srv = inputs.frame.instance_indices_srv](
                    const scene_pass_data& data,
                    const graphics::resource_bag& resources,
                    graphics::graphics_pass_context& context)
                {
                    const graphics::texture sized = data.depth.valid() ? data.depth : data.colors[0];
                    const std::uint32_t width = resources.width(sized);
                    const std::uint32_t height = resources.height(sized);
                    const auto layers = static_cast<std::uint16_t>(resources.description(sized).depth_or_layers);

                    // The first part's clears, of every layer
                    rhi::graphics_recorder& recorder = context.recorder();
                    for (std::size_t index = 0; index < data.color_count; ++index)
                    {
                        if (clears.colors[index])
                        {
                            rhi::texture_view all = resources.render_target_view(data.colors[index]);
                            all.layer_count = layers;
                            recorder.clear_render_target(all, *clears.colors[index]);
                        }
                    }
                    if (clears.depth && data.depth.valid())
                    {
                        rhi::texture_view all = resources.depth_stencil_view(data.depth);
                        all.layer_count = layers;
                        recorder.clear_depth(all, *clears.depth);
                    }

                    pass_bindings bindings;
                    bindings.depth_written = data.depth.valid();
                    bindings.pass_constants = context.allocate_constants(
                        gpu::scene_pass_constants{ .instances = instances_srv.value,
                                                   .instance_indices = instance_indices_srv.value,
                                                   .vertices = resources.srv(data.vertices).value,
                                                   .indices = resources.srv(data.indices).value,
                                                   .materials = resources.srv(data.materials).value,
                                                   .line_margin = line_margin,
                                                   .padding1 = 0,
                                                   .padding2 = 0 });
                    // Each viewport's targets: its layer of every attachment
                    std::array<std::array<rhi::texture_view, 8>, 8> colors{};
                    for (const scene_viewport& point : viewports)
                    {
                        std::array<rhi::texture_view, 8>& targets = colors[point.point.index];
                        for (std::size_t index = 0; index < data.color_count; ++index)
                        {
                            targets[index] = resources.render_target_view(data.colors[index], 0, point.layer);
                        }
                        viewport_binding binding{
                            .render_targets = std::span<const rhi::texture_view>{ targets }.first(data.color_count),
                            .viewport = { 0.0f,
                                          0.0f,
                                          static_cast<float>(width),
                                          static_cast<float>(height),
                                          0.0f,
                                          1.0f },
                            .scissor = { 0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height) },
                            .view_constants = context.allocate_constants(point.view_constants),
                        };
                        if (data.depth.valid())
                        {
                            rhi::texture_view depth = resources.depth_stencil_view(data.depth);
                            depth.first_layer = static_cast<std::uint16_t>(point.layer);
                            binding.depth = depth;
                        }
                        bindings.viewports[point.point.index] = binding;
                    }

                    const range_walk_sources sources{ .list = *list,
                                                      .meshes = meshes,
                                                      .pipelines = *pipelines,
                                                      .index_buffer = resources.buffer_handle(data.indices) };
                    walk(runs, kind, context, sources, bindings);
                });
            outputs.colors = written.colors;
            outputs.depth = written.depth;
        }
        return outputs;
    }
} // namespace avis::visualizer::rendering
