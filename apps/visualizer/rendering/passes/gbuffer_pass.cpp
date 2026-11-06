#include "rendering/passes/gbuffer_pass.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/formats.h"

#include <string_view>

namespace avis::visualizer::rendering
{
    namespace
    {
        // gbuffer_preview.hlsl's draw constants
        struct preview_constants
        {
            std::array<float, 4> background;
            std::uint32_t albedo;
            std::uint32_t linear_depth;
            std::uint32_t ldr_color;
            std::uint32_t padding0;
            std::uint32_t width;
            std::uint32_t height;
        };

        struct preview_data
        {
            graphics::texture albedo;
            graphics::texture linear_depth;
            graphics::texture ldr_color;
        };

        // The targets' names, in attachment_formats(pass_kind::gbuffer) order
        constexpr std::array<std::string_view, 5> target_names{
            "gbuffer_albedo", "gbuffer_normal", "gbuffer_material", "gbuffer_emissive", "velocity"
        };
    } // namespace

    gbuffer_targets
        add_gbuffer_pass(graphics::render_graph& graph, const scene_pass_inputs& inputs, const graphics::texture depth)
    {
        const pass_attachment_formats formats = attachment_formats(pass_kind::gbuffer);
        scene_attachments attachments;
        attachments.color_count = formats.render_targets.size();
        for (std::size_t index = 0; index < attachments.color_count; ++index)
        {
            graphics::texture_description description{ .size = graphics::view_relative(1.0f),
                                                       .format = formats.render_targets[index] };
            description.clear_value = rhi::clear_value{}; // 0: no surface, no velocity
            attachments.colors[index] =
                scene_attachment{ .name = target_names[index], .description = description, .clear = true };
        }
        attachments.depth = scene_attachment{ .texture = depth };

        const scene_range_pass pass{
            .name = "gbuffer",
            .kind = pass_kind::gbuffer,
            .range = prefix_range(key_prefix(key_group{ .viewport = viewport::camera() })),
        };
        const scene_pass_outputs written = add_scene_range_pass(graph, pass, inputs, attachments);
        return { .albedo = written.colors[0],
                 .normal = written.colors[1],
                 .material = written.colors[2],
                 .emissive = written.colors[3],
                 .velocity = written.colors[4],
                 .depth = written.depth };
    }

    graphics::texture add_gbuffer_preview(
        graphics::render_graph& graph,
        const rhi::compute_pipeline_handle pipeline,
        const gbuffer_targets& gbuffer,
        const graphics::texture linear_depth,
        const std::array<float, 4>& background)
    {
        return graph
            .add_compute_pass<preview_data>(
                "tonemap",
                [&](graphics::render_pass_builder& builder, preview_data& data)
                {
                    data.albedo = builder.read(gbuffer.albedo, graphics::texture_read::srv_compute);
                    data.linear_depth = builder.read(linear_depth, graphics::texture_read::srv_compute);
                    data.ldr_color = builder.write(
                        builder.create_texture(
                            "ldr_color", { .size = graphics::view_relative(1.0f), .format = rhi::format::rgba8_unorm }),
                        graphics::texture_write::uav);
                },
                [pipeline, background](
                    const preview_data& data,
                    const graphics::resource_bag& resources,
                    graphics::compute_pass_context& context)
                {
                    const preview_constants constants{ .background = background,
                                                       .albedo = resources.srv(data.albedo).value,
                                                       .linear_depth = resources.srv(data.linear_depth).value,
                                                       .ldr_color = resources.uav(data.ldr_color).value,
                                                       .padding0 = 0,
                                                       .width = resources.width(data.ldr_color),
                                                       .height = resources.height(data.ldr_color) };
                    rhi::compute_recorder& recorder = context.recorder();
                    recorder.set_pipeline(pipeline);
                    graphics::binding_layout::push_draw_constants(recorder, constants);
                    recorder.dispatch((constants.width + 7) / 8, (constants.height + 7) / 8, 1);
                })
            .ldr_color;
    }
} // namespace avis::visualizer::rendering
