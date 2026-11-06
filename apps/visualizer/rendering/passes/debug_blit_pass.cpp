#include "rendering/passes/debug_blit_pass.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"

#include <algorithm>
#include <vector>

namespace avis::visualizer::rendering
{
    namespace
    {
        struct blit_data
        {
            graphics::texture ldr_color;
        };

        struct inspect_data
        {
            graphics::buffer output;
        };

        struct readback_data
        {
            graphics::buffer source;
            graphics::buffer readback;
        };

        // The constants of one blit, without its SRVs
        gpu::debug_blit_constants make_constants(const debug_blit_source& source, const graphics::texture_extent tile)
        {
            gpu::debug_blit_constants constants{};
            constants.source_kind = source.source_kind;
            constants.is_array = source.is_array ? 1u : 0u;
            constants.mode = static_cast<std::uint32_t>(source.selection.mode);
            constants.mip = source.selection.mip;
            constants.slice = source.selection.slice;
            constants.channels = source.selection.channels;
            constants.origin = { static_cast<int>(source.rectangle.x), static_cast<int>(source.rectangle.y) };
            constants.extent = { source.rectangle.width, source.rectangle.height };
            constants.source_size = { source.source_size.width, source.source_size.height };
            constants.tile_size = { tile.width, tile.height };
            constants.range_minimum = source.selection.range_minimum;
            constants.range_maximum = source.selection.range_maximum;
            return constants;
        }

        void bind_sources(
            gpu::debug_blit_constants& constants,
            const debug_blit_source& source,
            const graphics::texture linear_depth,
            const graphics::resource_bag& resources)
        {
            if (source.source_kind == gpu::debug_source_clusters)
            {
                constants.source = resources.srv(source.clusters).value;
                constants.linear_depth = resources.srv(linear_depth).value;
            }
            else
            {
                constants.source = resources.srv(source.texture).value;
            }
        }

        constexpr std::uint32_t debug_mode_count = 9;
        static_assert(static_cast<std::uint32_t>(debug_blit_mode::alpha_only) == gpu::debug_mode_alpha_only);
        static_assert(
            static_cast<std::uint32_t>(debug_blit_mode::cluster_light_count) == gpu::debug_mode_cluster_light_count);
        static_assert(gpu::debug_mode_alpha_only + 1 == debug_mode_count);
    } // namespace

    rhi::graphics_pipeline_state debug_blit_pipeline_state() noexcept
    {
        rhi::graphics_pipeline_state state;
        state.rasterizer.cull = rhi::cull_mode::none;
        state.depth = { .test = false, .write = false };
        state.render_target_count = 1;
        state.render_target_formats[0] = rhi::format::rgba8_unorm;
        state.topology = rhi::primitive_topology::triangle_list;
        return state;
    }

    debug_blit_outputs add_debug_blits(graphics::render_graph& graph, const debug_blit_inputs& inputs)
    {
        debug_blit_outputs outputs{ .ldr_color = inputs.ldr_color };
        if (inputs.blits.empty())
        {
            return outputs;
        }
        const bool any_clusters = std::ranges::any_of(
            inputs.blits,
            [](const debug_blit_source& source) { return source.source_kind == gpu::debug_source_clusters; });

        std::vector<debug_blit_source> blits{ inputs.blits.begin(), inputs.blits.end() };
        const blit_data& blitted = graph.add_graphics_pass<blit_data>(
            "debug_blit",
            [&](graphics::render_pass_builder& builder, blit_data& data)
            {
                for (const debug_blit_source& source : blits)
                {
                    if (source.source_kind == gpu::debug_source_clusters)
                    {
                        builder.read_for_debug(source.clusters, graphics::buffer_read::srv_pixel);
                    }
                    else
                    {
                        builder.read_for_debug(source.texture, graphics::texture_read::srv_pixel);
                    }
                }
                if (any_clusters)
                {
                    builder.read(inputs.linear_depth, graphics::texture_read::srv_pixel);
                }
                data.ldr_color = builder.write(inputs.ldr_color, graphics::texture_write::render_target);
            },
            [pipeline = inputs.blit,
             blits,
             linear_depth = inputs.linear_depth,
             tile = inputs.tile_size,
             view_constants = inputs.view_constants](
                const blit_data& data,
                const graphics::resource_bag& resources,
                graphics::graphics_pass_context& context)
            {
                const rhi::texture_view target = resources.render_target_view(data.ldr_color);
                rhi::graphics_recorder& recorder = context.recorder();
                recorder.set_render_targets(std::span{ &target, 1 }, {});
                recorder.set_pipeline(pipeline);
                graphics::binding_layout::set_view_constants(recorder, context.allocate_constants(view_constants));
                for (const debug_blit_source& source : blits)
                {
                    gpu::debug_blit_constants constants = make_constants(source, tile);
                    bind_sources(constants, source, linear_depth, resources);
                    const view_rectangle& rectangle = source.rectangle;
                    recorder.set_viewport(
                        { static_cast<float>(rectangle.x),
                          static_cast<float>(rectangle.y),
                          static_cast<float>(rectangle.width),
                          static_cast<float>(rectangle.height),
                          0.0f,
                          1.0f });
                    recorder.set_scissor(
                        { static_cast<std::int32_t>(rectangle.x),
                          static_cast<std::int32_t>(rectangle.y),
                          static_cast<std::int32_t>(rectangle.x + rectangle.width),
                          static_cast<std::int32_t>(rectangle.y + rectangle.height) });
                    graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                    recorder.draw(3);
                }
            });
        outputs.ldr_color = blitted.ldr_color;

        if (!inputs.inspection || inputs.inspection->blit >= blits.size())
        {
            return outputs;
        }
        const debug_inspection& inspection = *inputs.inspection;
        const debug_blit_source& inspected = blits[inspection.blit];
        const inspect_data& read = graph.add_compute_pass<inspect_data>(
            "debug_inspect",
            [&](graphics::render_pass_builder& builder, inspect_data& data)
            {
                if (inspected.source_kind == gpu::debug_source_clusters)
                {
                    builder.read(inspected.clusters, graphics::buffer_read::srv_compute);
                    builder.read(inputs.linear_depth, graphics::texture_read::srv_compute);
                }
                else
                {
                    builder.read(inspected.texture, graphics::texture_read::srv_compute);
                }
                data.output = builder.write(
                    builder.create_buffer("debug_inspector", { .size = gpu::debug_inspector_bytes, .raw = true }),
                    graphics::buffer_write::uav);
            },
            [pipeline = inputs.inspect,
             source = inspected,
             texel = inspection.texel,
             linear_depth = inputs.linear_depth,
             tile = inputs.tile_size,
             view_constants = inputs.view_constants](
                const inspect_data& data,
                const graphics::resource_bag& resources,
                graphics::compute_pass_context& context)
            {
                gpu::debug_blit_constants constants = make_constants(source, tile);
                bind_sources(constants, source, linear_depth, resources);
                constants.texel = { texel.x, texel.y };
                constants.output = resources.uav(data.output).value;
                rhi::compute_recorder& recorder = context.recorder();
                recorder.set_pipeline(pipeline);
                graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                graphics::binding_layout::set_view_constants(recorder, context.allocate_constants(view_constants));
                recorder.dispatch(1, 1, 1);
            });
        const readback_data& copied = graph.add_copy_pass<readback_data>(
            "debug_readback",
            [&](graphics::render_pass_builder& builder, readback_data& data)
            {
                data.source = builder.read(read.output, graphics::buffer_read::copy_source);
                data.readback = builder.write(inspection.readback, graphics::buffer_write::copy_destination);
            },
            [](const readback_data& data, const graphics::resource_bag& bag, graphics::copy_pass_context& context)
            {
                context.recorder().copy_buffer(
                    bag.buffer_handle(data.readback), 0, bag.buffer_handle(data.source), 0, gpu::debug_inspector_bytes);
            });
        outputs.readback = copied.readback;
        return outputs;
    }

    graphics::buffer add_cluster_statistics(
        graphics::render_graph& graph,
        const rhi::compute_pipeline_handle pipeline,
        const graphics::buffer cluster_lights,
        const graphics::buffer readback)
    {
        struct statistics_data
        {
            graphics::buffer cluster_lights;
            graphics::buffer output;
        };
        const statistics_data& summed = graph.add_compute_pass<statistics_data>(
            "cluster_statistics",
            [&](graphics::render_pass_builder& builder, statistics_data& data)
            {
                data.cluster_lights = builder.read(cluster_lights, graphics::buffer_read::srv_compute);
                data.output = builder.write(
                    builder.create_buffer("cluster_statistics", { .size = gpu::cluster_statistics_bytes, .raw = true }),
                    graphics::buffer_write::uav);
            },
            [pipeline](
                const statistics_data& data,
                const graphics::resource_bag& resources,
                graphics::compute_pass_context& context)
            {
                const gpu::cluster_statistics_constants constants{ .cluster_lights =
                                                                       resources.srv(data.cluster_lights).value,
                                                                   .output = resources.uav(data.output).value };
                rhi::compute_recorder& recorder = context.recorder();
                recorder.set_pipeline(pipeline);
                graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                recorder.dispatch(1, 1, 1);
            });
        const readback_data& copied = graph.add_copy_pass<readback_data>(
            "cluster_statistics_readback",
            [&](graphics::render_pass_builder& builder, readback_data& data)
            {
                data.source = builder.read(summed.output, graphics::buffer_read::copy_source);
                data.readback = builder.write(readback, graphics::buffer_write::copy_destination);
            },
            [](const readback_data& data, const graphics::resource_bag& bag, graphics::copy_pass_context& context)
            {
                context.recorder().copy_buffer(
                    bag.buffer_handle(data.readback),
                    0,
                    bag.buffer_handle(data.source),
                    0,
                    gpu::cluster_statistics_bytes);
            });
        return copied.readback;
    }
} // namespace avis::visualizer::rendering
