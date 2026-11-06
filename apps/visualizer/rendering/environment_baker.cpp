#include "rendering/environment_baker.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/formats.h"

#include <algorithm>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        // environment_bake.hlsl's draw constants
        struct bake_constants
        {
            std::uint32_t source = 0;
            std::uint32_t destination = 0;
            std::uint32_t size = 0;
            std::uint32_t sample_count = 0;
            float roughness = 0.0f;
            std::uint32_t padding0 = 0;
            std::uint32_t padding1 = 0;
            std::uint32_t padding2 = 0;
        };
        static_assert(sizeof(bake_constants) == 32);

        constexpr std::uint32_t groups(const std::uint32_t extent) noexcept
        {
            return (extent + 7) / 8;
        }

        struct sky_bake_data
        {
            graphics::texture sky;
        };

        struct prefilter_data
        {
            graphics::texture sky;
            graphics::texture prefiltered;
        };

        struct irradiance_data
        {
            graphics::texture sky;
            graphics::buffer irradiance;
        };

        struct brdf_lut_data
        {
            graphics::texture lut;
        };
    } // namespace

    result<std::unique_ptr<environment_baker>>
        environment_baker::create(rhi::resources resources, const environment_pipelines& pipelines)
    {
        std::unique_ptr<environment_baker> baker{ new environment_baker{ resources, pipelines } };
        const auto usage = rhi::texture_usage::shader_resource | rhi::texture_usage::unordered_access;

        auto sky = resources.create_texture(
            { .dimension = rhi::texture_dimension::cube,
              .width = gpu::environment_sky_size,
              .height = gpu::environment_sky_size,
              .format = rhi::format::rgba16_float,
              .usage = usage,
              .debug_name = "environment_sky" });
        if (!sky)
        {
            return std::unexpected{ std::move(sky).error() };
        }
        baker->sky_texture_ = *sky;

        auto prefiltered = resources.create_texture(
            { .dimension = rhi::texture_dimension::cube,
              .width = gpu::environment_prefiltered_size,
              .height = gpu::environment_prefiltered_size,
              .mip_levels = gpu::environment_prefiltered_mips,
              .format = rhi::format::rgba16_float,
              .usage = usage,
              .debug_name = "environment_prefiltered" });
        if (!prefiltered)
        {
            return std::unexpected{ std::move(prefiltered).error() };
        }
        baker->prefiltered_texture_ = *prefiltered;

        auto irradiance = resources.create_buffer(
            { .size = std::uint64_t{ gpu::environment_irradiance_coefficients } * 16,
              .structure_stride = 16,
              .usage = rhi::buffer_usage::shader_resource | rhi::buffer_usage::unordered_access,
              .memory = rhi::memory_type::gpu_only,
              .debug_name = "environment_irradiance" });
        if (!irradiance)
        {
            return std::unexpected{ std::move(irradiance).error() };
        }
        baker->irradiance_buffer_ = *irradiance;

        auto lut = resources.create_texture(
            { .width = gpu::brdf_lut_size,
              .height = gpu::brdf_lut_size,
              .format = rhi::format::rg16_float,
              .usage = usage,
              .debug_name = "brdf_lut" });
        if (!lut)
        {
            return std::unexpected{ std::move(lut).error() };
        }
        baker->brdf_lut_texture_ = *lut;
        return baker;
    }

    environment_baker::environment_baker(
        const rhi::resources resources, const environment_pipelines& pipelines) noexcept :
        resources_{ resources },
        pipelines_{ pipelines }
    {}

    environment_baker::~environment_baker()
    {
        destroy();
    }

    void environment_baker::destroy() noexcept
    {
        for (rhi::texture_handle* texture : { &sky_texture_, &prefiltered_texture_, &brdf_lut_texture_ })
        {
            if (texture->is_valid())
            {
                resources_.destroy(*texture);
                *texture = {};
            }
        }
        if (irradiance_buffer_.is_valid())
        {
            resources_.destroy(irradiance_buffer_);
            irradiance_buffer_ = {};
        }
    }

    environment_resources environment_baker::add_passes(
        graphics::render_graph& graph, const environment_description& environment, const bool bake)
    {
        environment_pending_ = false;
        brdf_lut_pending_ = false;
        sky_ = make_sky_constants(environment);

        const rhi::access_state common = rhi::access_states::common;
        environment_resources resources{
            .sky = graph.import_texture("environment_sky", sky_texture_, common, common),
            .prefiltered = graph.import_texture("environment_prefiltered", prefiltered_texture_, common, common),
            .irradiance = graph.import_buffer("environment_irradiance", irradiance_buffer_, common, common),
            .brdf_lut = graph.import_texture("brdf_lut", brdf_lut_texture_, common, common),
        };
        if (!bake)
        {
            return resources;
        }
        if (!brdf_lut_baked_)
        {
            add_brdf_lut_bake(graph, resources);
            brdf_lut_baked_ = true;
            brdf_lut_pending_ = true;
            ++statistics_.brdf_lut_bakes;
        }
        if (!baked_ || !(*baked_ == environment))
        {
            add_environment_bake(graph, resources);
            baked_ = environment;
            environment_pending_ = true;
            ++statistics_.bakes;
        }
        return resources;
    }

    void environment_baker::forget() noexcept
    {
        if (environment_pending_)
        {
            baked_.reset();
            environment_pending_ = false;
        }
        if (brdf_lut_pending_)
        {
            brdf_lut_baked_ = false;
            brdf_lut_pending_ = false;
        }
    }

    void environment_baker::add_environment_bake(graphics::render_graph& graph, environment_resources& resources)
    {
        resources.sky =
            graph
                .add_compute_pass<sky_bake_data>(
                    "bake_sky",
                    [&](graphics::render_pass_builder& builder, sky_bake_data& data)
                    { data.sky = builder.write(resources.sky, graphics::texture_write::uav); },
                    [pipeline = pipelines_.sky, sky = sky_](
                        const sky_bake_data& data,
                        const graphics::resource_bag& bag,
                        graphics::compute_pass_context& context)
                    {
                        const bake_constants constants{ .destination = bag.uav(data.sky, 0).value,
                                                        .size = gpu::environment_sky_size };
                        rhi::compute_recorder& recorder = context.recorder();
                        recorder.set_pipeline(pipeline);
                        graphics::binding_layout::push_draw_constants(recorder, constants);
                        graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(sky));
                        recorder.dispatch(groups(gpu::environment_sky_size), groups(gpu::environment_sky_size), 6);
                    })
                .sky;

        resources.prefiltered =
            graph
                .add_compute_pass<prefilter_data>(
                    "bake_prefiltered",
                    [&](graphics::render_pass_builder& builder, prefilter_data& data)
                    {
                        data.sky = builder.read(resources.sky, graphics::texture_read::srv_compute);
                        data.prefiltered = builder.write(resources.prefiltered, graphics::texture_write::uav);
                    },
                    [pipeline = pipelines_.prefilter](
                        const prefilter_data& data,
                        const graphics::resource_bag& bag,
                        graphics::compute_pass_context& context)
                    {
                        rhi::compute_recorder& recorder = context.recorder();
                        recorder.set_pipeline(pipeline);
                        for (std::uint32_t mip = 0; mip < gpu::environment_prefiltered_mips; ++mip)
                        {
                            const std::uint32_t size = std::max(gpu::environment_prefiltered_size >> mip, 1u);
                            const bake_constants constants{
                                .source = bag.srv(data.sky).value,
                                .destination = bag.uav(data.prefiltered, mip).value,
                                .size = size,
                                .sample_count = environment_prefilter_samples,
                                .roughness =
                                    static_cast<float>(mip) / static_cast<float>(gpu::environment_prefiltered_mips - 1),
                            };
                            graphics::binding_layout::push_draw_constants(recorder, constants);
                            recorder.dispatch(groups(size), groups(size), 6);
                        }
                    })
                .prefiltered;

        resources.irradiance = graph
                                   .add_compute_pass<irradiance_data>(
                                       "bake_irradiance",
                                       [&](graphics::render_pass_builder& builder, irradiance_data& data)
                                       {
                                           data.sky = builder.read(resources.sky, graphics::texture_read::srv_compute);
                                           data.irradiance =
                                               builder.write(resources.irradiance, graphics::buffer_write::uav);
                                       },
                                       [pipeline = pipelines_.irradiance](
                                           const irradiance_data& data,
                                           const graphics::resource_bag& bag,
                                           graphics::compute_pass_context& context)
                                       {
                                           const bake_constants constants{
                                               .source = bag.srv(data.sky).value,
                                               .destination = bag.uav(data.irradiance).value,
                                               .size = environment_irradiance_samples_per_side,
                                           };
                                           rhi::compute_recorder& recorder = context.recorder();
                                           recorder.set_pipeline(pipeline);
                                           graphics::binding_layout::push_draw_constants(recorder, constants);
                                           recorder.dispatch(1, 1, 1);
                                       })
                                   .irradiance;
    }

    void environment_baker::add_brdf_lut_bake(graphics::render_graph& graph, environment_resources& resources)
    {
        resources.brdf_lut = graph
                                 .add_compute_pass<brdf_lut_data>(
                                     "bake_brdf_lut",
                                     [&](graphics::render_pass_builder& builder, brdf_lut_data& data)
                                     { data.lut = builder.write(resources.brdf_lut, graphics::texture_write::uav); },
                                     [pipeline = pipelines_.brdf_lut](
                                         const brdf_lut_data& data,
                                         const graphics::resource_bag& bag,
                                         graphics::compute_pass_context& context)
                                     {
                                         const bake_constants constants{ .destination = bag.uav(data.lut, 0).value,
                                                                         .size = gpu::brdf_lut_size,
                                                                         .sample_count = brdf_lut_samples };
                                         rhi::compute_recorder& recorder = context.recorder();
                                         recorder.set_pipeline(pipeline);
                                         graphics::binding_layout::push_draw_constants(recorder, constants);
                                         recorder.dispatch(groups(gpu::brdf_lut_size), groups(gpu::brdf_lut_size), 1);
                                     })
                                 .lut;
    }
} // namespace avis::visualizer::rendering
