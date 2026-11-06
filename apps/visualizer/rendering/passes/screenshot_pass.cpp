#include "rendering/passes/screenshot_pass.h"

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
        constexpr std::uint32_t groups(const std::uint32_t extent, const std::uint32_t group) noexcept
        {
            return (extent + group - 1) / group;
        }

        struct exposure_data
        {
            graphics::buffer source;
            graphics::buffer exposure;
        };

        struct accumulate_data
        {
            graphics::texture color;
            graphics::texture accumulation;
            graphics::buffer exposure;
        };

        struct resolve_data
        {
            graphics::texture accumulation;
            graphics::buffer exposure;
            graphics::texture resolved;
        };

        struct copy_data
        {
            graphics::texture source;
            graphics::buffer readback;
        };
    } // namespace

    graphics::buffer add_screenshot_exposure(
        graphics::render_graph& graph, const graphics::buffer source, const graphics::buffer exposure)
    {
        return graph
            .add_copy_pass<exposure_data>(
                "screenshot_exposure",
                [&](graphics::render_pass_builder& builder, exposure_data& data)
                {
                    data.source = builder.read(source, graphics::buffer_read::copy_source);
                    data.exposure = builder.write(exposure, graphics::buffer_write::copy_destination);
                },
                [](const exposure_data& data, const graphics::resource_bag& bag, graphics::copy_pass_context& context)
                {
                    // The source's current exposure is the one its next frame pre-exposes with: both slots get it
                    const rhi::buffer_handle destination = bag.buffer_handle(data.exposure);
                    const rhi::buffer_handle from = bag.buffer_handle(data.source);
                    context.recorder().copy_buffer(destination, gpu::exposure_offset_current, from, 0, 4);
                    context.recorder().copy_buffer(destination, gpu::exposure_offset_previous, from, 0, 4);
                })
            .exposure;
    }

    graphics::texture add_screenshot_accumulate(
        graphics::render_graph& graph,
        const screenshot_pipelines& pipelines,
        const graphics::texture hdr_color,
        const graphics::texture accumulation,
        const graphics::buffer exposure,
        const bool first)
    {
        return graph
            .add_compute_pass<accumulate_data>(
                "screenshot_accumulate",
                [&](graphics::render_pass_builder& builder, accumulate_data& data)
                {
                    data.color = builder.read(hdr_color, graphics::texture_read::srv_compute);
                    data.exposure = builder.read(exposure, graphics::buffer_read::srv_compute);
                    data.accumulation = builder.write(accumulation, graphics::texture_write::uav);
                },
                [pipeline = pipelines.accumulate, first](
                    const accumulate_data& data,
                    const graphics::resource_bag& resources,
                    graphics::compute_pass_context& context)
                {
                    gpu::screenshot_constants constants{};
                    constants.size = { resources.width(data.accumulation), resources.height(data.accumulation) };
                    constants.color = resources.srv(data.color).value;
                    constants.accumulation = resources.uav(data.accumulation).value;
                    constants.exposure_buffer = resources.srv(data.exposure).value;
                    constants.first = first ? 1u : 0u;
                    rhi::compute_recorder& recorder = context.recorder();
                    recorder.set_pipeline(pipeline);
                    graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                    recorder.dispatch(groups(constants.size.x, 8), groups(constants.size.y, 8), 1);
                })
            .accumulation;
    }

    graphics::texture add_screenshot_resolve(
        graphics::render_graph& graph,
        const screenshot_pipelines& pipelines,
        const graphics::texture accumulation,
        const graphics::buffer exposure,
        const std::uint32_t subframes)
    {
        const float weight = 1.0f / static_cast<float>(subframes > 0 ? subframes : 1);
        return graph
            .add_compute_pass<resolve_data>(
                "screenshot_resolve",
                [&](graphics::render_pass_builder& builder, resolve_data& data)
                {
                    data.accumulation = builder.read(accumulation, graphics::texture_read::srv_compute);
                    data.exposure = builder.read(exposure, graphics::buffer_read::srv_compute);
                    data.resolved = builder.write(
                        builder.create_texture(
                            "screenshot_resolved",
                            { .size = graphics::view_relative(1.0f), .format = rhi::format::rgba32_float }),
                        graphics::texture_write::uav);
                },
                [pipeline = pipelines.resolve, weight](
                    const resolve_data& data,
                    const graphics::resource_bag& resources,
                    graphics::compute_pass_context& context)
                {
                    gpu::screenshot_constants constants{};
                    constants.size = { resources.width(data.resolved), resources.height(data.resolved) };
                    constants.accumulation = resources.srv(data.accumulation).value;
                    constants.exposure_buffer = resources.srv(data.exposure).value;
                    constants.resolved = resources.uav(data.resolved).value;
                    constants.weight = weight;
                    rhi::compute_recorder& recorder = context.recorder();
                    recorder.set_pipeline(pipeline);
                    graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                    recorder.dispatch(groups(constants.size.x, 8), groups(constants.size.y, 8), 1);
                })
            .resolved;
    }

    void add_screenshot_copy(
        graphics::render_graph& graph, const graphics::texture ldr_color, const graphics::buffer readback)
    {
        graph.add_copy_pass<copy_data>(
            "screenshot_capture",
            [&](graphics::render_pass_builder& builder, copy_data& data)
            {
                data.source = builder.read(ldr_color, graphics::texture_read::copy_source);
                data.readback = builder.write(readback, graphics::buffer_write::copy_destination);
            },
            [](const copy_data& data, const graphics::resource_bag& bag, graphics::copy_pass_context& context)
            {
                context.recorder().copy_texture_to_buffer(
                    bag.buffer_handle(data.readback), 0, bag.texture_handle(data.source), {});
            });
    }
} // namespace avis::visualizer::rendering
