#include "rendering/passes/exposure_passes.h"

#include "rendering/passes/lighting_passes.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/formats.h"

#include <cstdint>
#include <string_view>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::uint32_t groups(const std::uint32_t extent, const std::uint32_t group) noexcept
        {
            return (extent + group - 1) / group;
        }

        struct adapt_data
        {
            graphics::buffer exposure;
        };

        struct histogram_data
        {
            graphics::texture color;
            graphics::buffer exposure;
        };

        struct tonemap_data
        {
            graphics::texture color;
            graphics::buffer exposure;
            graphics::texture blue_noise;
            graphics::texture ldr_color;
        };

        // exposure.hlsl's adapt_main with `constants`, whose exposure_buffer is set when recording
        graphics::buffer add_adapt(
            graphics::render_graph& graph,
            const std::string_view name,
            const rhi::compute_pipeline_handle pipeline,
            const graphics::buffer exposure,
            const gpu::exposure_constants& constants)
        {
            return graph
                .add_compute_pass<adapt_data>(
                    name,
                    [&](graphics::render_pass_builder& builder, adapt_data& data)
                    { data.exposure = builder.write(exposure, graphics::buffer_write::uav); },
                    [pipeline, constants](
                        const adapt_data& data,
                        const graphics::resource_bag& resources,
                        graphics::compute_pass_context& context)
                    {
                        gpu::exposure_constants bound = constants;
                        bound.exposure_buffer = resources.uav(data.exposure).value;
                        rhi::compute_recorder& recorder = context.recorder();
                        recorder.set_pipeline(pipeline);
                        graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(bound));
                        recorder.dispatch(1, 1, 1);
                    })
                .exposure;
        }
    } // namespace

    graphics::buffer add_exposure_reset(
        graphics::render_graph& graph,
        const rhi::compute_pipeline_handle adapt,
        const graphics::buffer exposure,
        const float initial)
    {
        gpu::exposure_constants constants{};
        constants.mode = gpu::exposure_mode_reset;
        constants.manual_exposure = initial;
        return add_adapt(graph, "exposure_reset", adapt, exposure, constants);
    }

    graphics::buffer add_exposure_adaptation(graphics::render_graph& graph, const exposure_inputs& inputs)
    {
        graphics::buffer exposure = inputs.exposure;
        const bool automatic = inputs.settings.mode == exposure_mode::automatic;
        if (automatic)
        {
            const histogram_data& histogram = graph.add_compute_pass<histogram_data>(
                "exposure_histogram",
                [&](graphics::render_pass_builder& builder, histogram_data& data)
                {
                    data.color = builder.read(inputs.color, graphics::texture_read::srv_compute);
                    data.exposure = builder.write(exposure, graphics::buffer_write::uav);
                },
                [pipeline = inputs.histogram](
                    const histogram_data& data,
                    const graphics::resource_bag& resources,
                    graphics::compute_pass_context& context)
                {
                    gpu::exposure_constants constants{};
                    constants.exposure_buffer = resources.uav(data.exposure).value;
                    constants.color = resources.srv(data.color).value;
                    constants.size = { resources.width(data.color), resources.height(data.color) };
                    constants.key = exposure_key;
                    rhi::compute_recorder& recorder = context.recorder();
                    recorder.set_pipeline(pipeline);
                    graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                    recorder.dispatch(groups(constants.size.x, 16), groups(constants.size.y, 16), 1);
                });
            exposure = histogram.exposure;
        }

        gpu::exposure_constants constants{};
        constants.mode = automatic ? gpu::exposure_mode_automatic : gpu::exposure_mode_manual;
        constants.key = exposure_key;
        constants.manual_exposure = manual_exposure(inputs.settings.ev100);
        constants.frame_time = inputs.frame_time;
        constants.brighten_rate = exposure_brighten_rate;
        constants.darken_rate = exposure_darken_rate;
        // A higher EV100 is a lower exposure
        constants.minimum_exposure = manual_exposure(exposure_maximum_ev100);
        constants.maximum_exposure = manual_exposure(exposure_minimum_ev100);
        return add_adapt(graph, "exposure_adapt", inputs.adapt, exposure, constants);
    }

    graphics::texture add_tonemap(graphics::render_graph& graph, const tonemap_inputs& inputs)
    {
        const std::uint32_t tonemapper =
            inputs.tonemapper == tonemap_operator::agx ? gpu::tonemapper_agx : gpu::tonemapper_aces;
        return graph
            .add_compute_pass<tonemap_data>(
                "tonemap",
                [&](graphics::render_pass_builder& builder, tonemap_data& data)
                {
                    data.color = builder.read(inputs.color, graphics::texture_read::srv_compute);
                    data.exposure = builder.read(inputs.exposure, graphics::buffer_read::srv_compute);
                    data.blue_noise = builder.read(inputs.blue_noise, graphics::texture_read::srv_compute);
                    data.ldr_color = builder.write(
                        builder.create_texture(
                            "ldr_color", { .size = graphics::view_relative(1.0f), .format = rhi::format::rgba8_unorm }),
                        graphics::texture_write::uav);
                },
                [pipeline = inputs.pipeline, tonemapper](
                    const tonemap_data& data,
                    const graphics::resource_bag& resources,
                    graphics::compute_pass_context& context)
                {
                    gpu::tonemap_constants constants{};
                    constants.color = resources.srv(data.color).value;
                    constants.ldr_color = resources.uav(data.ldr_color).value;
                    constants.exposure_buffer = resources.srv(data.exposure).value;
                    constants.blue_noise = resources.srv(data.blue_noise).value;
                    constants.size = { resources.width(data.ldr_color), resources.height(data.ldr_color) };
                    constants.tonemapper = tonemapper;
                    rhi::compute_recorder& recorder = context.recorder();
                    recorder.set_pipeline(pipeline);
                    graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                    recorder.dispatch(groups(constants.size.x, 8), groups(constants.size.y, 8), 1);
                })
            .ldr_color;
    }
} // namespace avis::visualizer::rendering
