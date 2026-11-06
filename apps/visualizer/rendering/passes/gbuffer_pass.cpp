#include "rendering/passes/gbuffer_pass.h"

#include "avis/rhi/formats.h"

#include <array>
#include <string_view>

namespace avis::visualizer::rendering
{
    namespace
    {
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
} // namespace avis::visualizer::rendering
