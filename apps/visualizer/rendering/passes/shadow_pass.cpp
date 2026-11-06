#include "rendering/passes/shadow_pass.h"

#include "avis/rhi/formats.h"

#include <array>

namespace avis::visualizer::rendering
{
    graphics::texture add_shadow_cascades(
        graphics::render_graph& graph,
        const scene_pass_inputs& inputs,
        const shadow_cascades& cascades,
        const std::uint32_t frame_index)
    {
        graphics::texture_description description{ .size = graphics::absolute(cascades.resolution, cascades.resolution),
                                                   .depth_or_layers = cascades.count,
                                                   .format = rhi::format::d32_float };
        description.clear_value = rhi::clear_value{ .depth = 0.0f }; // reverse-Z: nothing in front of the far side
        scene_attachments attachments;
        attachments.depth = scene_attachment{ .name = "shadow_map", .description = description, .clear = true };

        std::array<scene_viewport, maximum_shadow_cascades> viewports{};
        for (std::uint32_t cascade = 0; cascade < cascades.count; ++cascade)
        {
            viewports[cascade] = { .point = viewport::shadow_cascade(cascade),
                                   .layer = cascade,
                                   .view_constants = make_cascade_view_constants(cascades, cascade, frame_index) };
        }

        const scene_range_pass pass{
            .name = "shadow_cascades",
            .kind = pass_kind::shadow,
            .range = prefix_range(
                key_prefix(key_group{ .viewport = viewport::shadow_cascade(0) }),
                key_prefix(key_group{ .viewport = viewport::shadow_cascade(cascades.count - 1) })),
            .viewports = std::span<const scene_viewport>{ viewports }.first(cascades.count),
        };
        return add_scene_range_pass(graph, pass, inputs, attachments).depth;
    }
} // namespace avis::visualizer::rendering
