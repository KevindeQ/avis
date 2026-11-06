#include "rendering/passes/overlay_pass.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/linear_arena.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/formats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::uint32_t groups(const std::uint32_t extent) noexcept
        {
            return (extent + 7) / 8;
        }

        // The exact sRGB transfer function (IEC 61966-2-1), as overlay.hlsl's linear_to_srgb
        float srgb_encode(const float value) noexcept
        {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            return clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
        }

        key_range layer_range(const viewport_layer layer) noexcept
        {
            return prefix_range(
                key_prefix(
                    key_group{ .viewport = viewport::camera(),
                               .viewport_layer = layer,
                               .translucency = translucency::opaque }),
                key_prefix(
                    key_group{ .viewport = viewport::camera(),
                               .viewport_layer = layer,
                               .translucency = translucency::subtractive }));
        }

        struct outline_data
        {
            graphics::texture selection_mask;
            graphics::texture ldr_color;
        };
    } // namespace

    key_range overlay_range() noexcept
    {
        return layer_range(viewport_layer::overlay);
    }

    key_range overlay_on_top_range() noexcept
    {
        return layer_range(viewport_layer::overlay_on_top);
    }

    key_range selection_range() noexcept
    {
        return { layer_range(viewport_layer::world).first, layer_range(viewport_layer::overlay).last };
    }

    std::span<const draw_run>
        selected_runs(const prepared_draw_list& list, const key_range range, graphics::linear_arena& arena)
    {
        const std::span<const draw_item> items = list.list->items();
        const std::span<const draw_record> records = list.list->records();
        const std::span<const draw_run> runs = list.runs_of(list.list->range(range));
        std::vector<draw_run> selected;
        for (const draw_run& run : runs)
        {
            if (is_command_key(items[run.first].key))
            {
                continue;
            }
            const bool any_selected = std::ranges::any_of(
                items.subspan(run.first, run.count),
                [&](const draw_item& item)
                { return static_cast<bool>(records[item.value].flags & scene::instance_flags::selected); });
            if (any_selected)
            {
                selected.push_back(run);
            }
        }
        if (selected.empty())
        {
            return {};
        }
        const std::span<draw_run> stored = arena.allocate_array<draw_run>(selected.size());
        std::ranges::copy(selected, stored.begin());
        return stored;
    }

    overlay_outputs
        add_overlay(graphics::render_graph& graph, const scene_pass_inputs& inputs, const overlay_inputs& overlay)
    {
        overlay_outputs outputs{ .ldr_color = overlay.ldr_color, .depth = overlay.depth };

        // The selection mask and its outline, under the overlays, on frames with a selected instance only
        const std::span<const draw_run> selected = selected_runs(inputs.list, selection_range(), graph.arena());
        const std::span<const draw_run> selected_on_top =
            selected_runs(inputs.list, overlay_on_top_range(), graph.arena());
        if (!selected.empty() || !selected_on_top.empty())
        {
            const pass_attachment_formats formats = attachment_formats(pass_kind::selection);
            graphics::texture_description mask{ .size = graphics::view_relative(1.0f),
                                                .format = formats.render_targets[0] };
            mask.clear_value = rhi::clear_value{}; // 0: nothing selected
            scene_attachments attachments;
            attachments.color_count = 1;
            attachments.colors[0] = scene_attachment{ .name = "selection_mask", .description = mask, .clear = true };
            attachments.depth = scene_attachment{ .texture = outputs.depth };
            // The first pass creates and clears the mask, even with no depth-tested selected run
            scene_pass_outputs written = add_scene_range_pass(
                graph,
                { .name = "selection_mask", .kind = pass_kind::selection, .runs = selected },
                inputs,
                attachments);
            if (!selected_on_top.empty())
            {
                attachments.colors[0] = scene_attachment{ .texture = written.colors[0] };
                attachments.depth = scene_attachment{ .texture = written.depth };
                written = add_scene_range_pass(
                    graph,
                    { .name = "selection_mask_on_top", .kind = pass_kind::selection_on_top, .runs = selected_on_top },
                    inputs,
                    attachments);
            }
            outputs.selection_mask = written.colors[0];
            outputs.depth = written.depth;

            const std::array color{ srgb_encode(overlay.selection_color.red),
                                    srgb_encode(overlay.selection_color.green),
                                    srgb_encode(overlay.selection_color.blue),
                                    1.0f };
            outputs.ldr_color =
                graph
                    .add_compute_pass<outline_data>(
                        "selection_outline",
                        [&](graphics::render_pass_builder& builder, outline_data& data)
                        {
                            data.selection_mask =
                                builder.read(outputs.selection_mask, graphics::texture_read::srv_compute);
                            data.ldr_color = builder.write(outputs.ldr_color, graphics::texture_write::uav);
                        },
                        [pipeline = overlay.outline, color](
                            const outline_data& data,
                            const graphics::resource_bag& resources,
                            graphics::compute_pass_context& context)
                        {
                            gpu::selection_outline_constants constants{};
                            constants.selection_mask = resources.srv(data.selection_mask).value;
                            constants.ldr_color = resources.uav(data.ldr_color).value;
                            constants.size = { resources.width(data.ldr_color), resources.height(data.ldr_color) };
                            constants.color = { color[0], color[1], color[2], color[3] };
                            rhi::compute_recorder& recorder = context.recorder();
                            recorder.set_pipeline(pipeline);
                            graphics::binding_layout::set_pass_constants(
                                recorder, context.allocate_constants(constants));
                            recorder.dispatch(groups(constants.size.x), groups(constants.size.y), 1);
                        })
                    .ldr_color;
        }

        // The overlays: depth-tested, then on top. Lines get a pixel of margin for their edge falloff (11 §9.2).
        scene_attachments attachments;
        attachments.color_count = 1;
        attachments.colors[0] = scene_attachment{ .texture = outputs.ldr_color };
        attachments.depth = scene_attachment{ .texture = outputs.depth };
        const scene_pass_outputs tested = add_scene_range_pass(
            graph,
            { .name = "overlay", .kind = pass_kind::overlay, .range = overlay_range(), .line_margin = 1.0f },
            inputs,
            attachments);
        attachments.colors[0] = scene_attachment{ .texture = tested.colors[0] };
        attachments.depth = scene_attachment{ .texture = tested.depth };
        const scene_pass_outputs on_top = add_scene_range_pass(
            graph,
            { .name = "overlay_on_top",
              .kind = pass_kind::overlay_on_top,
              .range = overlay_on_top_range(),
              .line_margin = 1.0f },
            inputs,
            attachments);
        outputs.ldr_color = on_top.colors[0];
        outputs.depth = on_top.depth;
        return outputs;
    }
} // namespace avis::visualizer::rendering
