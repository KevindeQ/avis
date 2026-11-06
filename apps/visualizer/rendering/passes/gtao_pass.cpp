#include "rendering/passes/gtao_pass.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/formats.h"

#include <format>
#include <string>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::uint32_t groups(const std::uint32_t extent) noexcept
        {
            return (extent + 7) / 8;
        }

        struct gtao_data
        {
            graphics::texture depth_mips;
            graphics::texture normal;
            graphics::texture raw;
            graphics::texture edges;
        };

        struct denoise_data
        {
            graphics::texture source;
            graphics::texture edges;
            graphics::texture destination;
        };
    } // namespace

    graphics::texture_extent
        gtao_extent(const ambient_occlusion_settings& parameters, const graphics::texture_extent view)
    {
        const std::uint32_t scale = gtao_scale(parameters);
        return { (view.width + scale - 1) / scale, (view.height + scale - 1) / scale };
    }

    gtao_outputs add_gtao(graphics::render_graph& graph, const gtao_inputs& inputs)
    {
        const std::uint32_t scale = gtao_scale(inputs.parameters);
        // view_relative rounds up, as gtao_extent does
        const graphics::texture_size size = graphics::view_relative(1.0f / static_cast<float>(scale));
        const auto async = graphics::compute_queue_hint::async_compute;

        const gtao_data& traced = graph.add_compute_pass<gtao_data>(
            "gtao",
            [&](graphics::render_pass_builder& builder, gtao_data& data)
            {
                data.depth_mips = builder.read(inputs.depth_mips, graphics::texture_read::srv_compute);
                data.normal = builder.read(inputs.normal, graphics::texture_read::srv_compute);
                data.raw = builder.write(
                    builder.create_texture("ao_raw", { .size = size, .format = rhi::format::r8_unorm }),
                    graphics::texture_write::uav);
                data.edges = builder.write(
                    builder.create_texture("ao_edges", { .size = size, .format = rhi::format::r8_unorm }),
                    graphics::texture_write::uav);
            },
            [pipeline = inputs.pipelines.gtao,
             scale,
             mips = inputs.depth_mip_count,
             parameters = inputs.parameters,
             view_constants = inputs.view_constants](
                const gtao_data& data, const graphics::resource_bag& resources, graphics::compute_pass_context& context)
            {
                gpu::gtao_constants constants{};
                constants.depth_mips = resources.srv(data.depth_mips).value;
                constants.normal = resources.srv(data.normal).value;
                constants.ao = resources.uav(data.raw).value;
                constants.edges = resources.uav(data.edges).value;
                constants.size = { resources.width(data.raw), resources.height(data.raw) };
                constants.scale = scale;
                constants.depth_mip_count = mips;
                constants.slice_count = parameters.slices;
                constants.steps_per_side = parameters.steps;
                constants.radius = parameters.radius;
                constants.falloff = parameters.falloff;
                constants.intensity = parameters.intensity;

                rhi::compute_recorder& recorder = context.recorder();
                recorder.set_pipeline(pipeline);
                graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                graphics::binding_layout::set_view_constants(recorder, context.allocate_constants(view_constants));
                recorder.dispatch(groups(constants.size.x), groups(constants.size.y), 1);
            },
            async);

        graphics::texture filtered = traced.raw;
        for (std::uint32_t pass = 0; pass < gpu::gtao_denoise_passes; ++pass)
        {
            const bool last = pass + 1 == gpu::gtao_denoise_passes;
            const std::string target = last ? std::string{ "ao" } : std::format("ao_denoised_{}", pass);
            filtered =
                graph
                    .add_compute_pass<denoise_data>(
                        std::format("gtao_denoise_{}", pass),
                        [&](graphics::render_pass_builder& builder, denoise_data& data)
                        {
                            data.source = builder.read(filtered, graphics::texture_read::srv_compute);
                            data.edges = builder.read(traced.edges, graphics::texture_read::srv_compute);
                            data.destination = builder.write(
                                builder.create_texture(target, { .size = size, .format = rhi::format::r8_unorm }),
                                graphics::texture_write::uav);
                        },
                        [pipeline = inputs.pipelines.denoise](
                            const denoise_data& data,
                            const graphics::resource_bag& resources,
                            graphics::compute_pass_context& context)
                        {
                            gpu::gtao_denoise_constants constants{};
                            constants.source = resources.srv(data.source).value;
                            constants.edges = resources.srv(data.edges).value;
                            constants.destination = resources.uav(data.destination).value;
                            constants.size = { resources.width(data.destination), resources.height(data.destination) };
                            rhi::compute_recorder& recorder = context.recorder();
                            recorder.set_pipeline(pipeline);
                            graphics::binding_layout::push_draw_constants(recorder, constants);
                            recorder.dispatch(groups(constants.size.x), groups(constants.size.y), 1);
                        },
                        async)
                    .destination;
        }
        return { .raw = traced.raw, .edges = traced.edges, .ao = filtered, .scale = scale };
    }
} // namespace avis::visualizer::rendering
