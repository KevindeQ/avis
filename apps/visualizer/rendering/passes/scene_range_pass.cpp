#include "rendering/passes/scene_range_pass.h"

#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"

#include <format>
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
                 view_constants = inputs.view_constants,
                 instances_srv = inputs.frame.instances_srv,
                 instance_indices_srv = inputs.frame.instance_indices_srv](
                    const scene_pass_data& data,
                    const graphics::resource_bag& resources,
                    graphics::graphics_pass_context& context)
                {
                    std::array<rhi::texture_view, 8> colors{};
                    for (std::size_t index = 0; index < data.color_count; ++index)
                    {
                        colors[index] = resources.render_target_view(data.colors[index]);
                    }
                    const graphics::texture sized = data.depth.valid() ? data.depth : data.colors[0];
                    const std::uint32_t width = resources.width(sized);
                    const std::uint32_t height = resources.height(sized);

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
                    viewport_binding camera{
                        .render_targets = std::span<const rhi::texture_view>{ colors }.first(data.color_count),
                        .viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f },
                        .scissor = { 0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height) },
                        .view_constants = context.allocate_constants(view_constants),
                    };
                    if (data.depth.valid())
                    {
                        camera.depth = resources.depth_stencil_view(data.depth);
                    }
                    bindings.viewports[viewport::camera().index] = camera;

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
