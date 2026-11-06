#include "rendering/passes/picking_pass.h"

#include "rendering/passes/overlay_pass.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/formats.h"

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    namespace
    {
        struct window_data
        {
            graphics::texture picking_id;
            graphics::buffer window;
        };

        struct readback_data
        {
            graphics::buffer window;
            graphics::buffer readback;
        };
    } // namespace

    picking_outputs
        add_picking(graphics::render_graph& graph, const scene_pass_inputs& inputs, const picking_inputs& picking)
    {
        // The pickable instances of every camera layer, in key order: world and overlay with the overlay's depth test,
        // then overlay_on_top without one
        const pass_attachment_formats formats = attachment_formats(pass_kind::picking);
        graphics::texture_description target{ .size = graphics::view_relative(1.0f),
                                              .format = formats.render_targets[0] };
        target.clear_value = rhi::clear_value{}; // 0: nothing
        scene_attachments attachments;
        attachments.color_count = 1;
        attachments.colors[0] = scene_attachment{ .name = "picking_id", .description = target, .clear = true };
        attachments.depth = scene_attachment{ .texture = picking.depth };
        const scene_pass_outputs tested = add_scene_range_pass(
            graph,
            { .name = "picking", .kind = pass_kind::picking, .range = selection_range(), .line_margin = 1.0f },
            inputs,
            attachments);
        attachments.colors[0] = scene_attachment{ .texture = tested.colors[0] };
        attachments.depth = scene_attachment{ .texture = tested.depth };
        const scene_pass_outputs on_top = add_scene_range_pass(
            graph,
            { .name = "picking_on_top",
              .kind = pass_kind::picking_on_top,
              .range = overlay_on_top_range(),
              .line_margin = 1.0f },
            inputs,
            attachments);

        // The window around the click, then into the readback buffer
        const pick_window_origin origin = pick_origin(picking.request);
        const window_data& gathered = graph.add_compute_pass<window_data>(
            "pick_window",
            [&](graphics::render_pass_builder& builder, window_data& data)
            {
                data.picking_id = builder.read(on_top.colors[0], graphics::texture_read::srv_compute);
                data.window = builder.write(
                    builder.create_buffer("pick_window", { .size = pick_window_bytes, .raw = true }),
                    graphics::buffer_write::uav);
            },
            [pipeline = picking.window, origin](
                const window_data& data,
                const graphics::resource_bag& resources,
                graphics::compute_pass_context& context)
            {
                gpu::pick_window_constants constants{};
                constants.picking_id = resources.srv(data.picking_id).value;
                constants.window = resources.uav(data.window).value;
                constants.origin = { origin.x, origin.y };
                constants.size = { resources.width(data.picking_id), resources.height(data.picking_id) };
                rhi::compute_recorder& recorder = context.recorder();
                recorder.set_pipeline(pipeline);
                graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                recorder.dispatch(1, 1, 1);
            });
        const readback_data& copied = graph.add_copy_pass<readback_data>(
            "pick_readback",
            [&](graphics::render_pass_builder& builder, readback_data& data)
            {
                data.window = builder.read(gathered.window, graphics::buffer_read::copy_source);
                data.readback = builder.write(picking.readback, graphics::buffer_write::copy_destination);
            },
            [](const readback_data& data, const graphics::resource_bag& bag, graphics::copy_pass_context& context)
            {
                context.recorder().copy_buffer(
                    bag.buffer_handle(data.readback), 0, bag.buffer_handle(data.window), 0, pick_window_bytes);
            });
        return { .picking_id = on_top.colors[0], .depth = on_top.depth, .readback = copied.readback };
    }
} // namespace avis::visualizer::rendering
