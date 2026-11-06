#include "rendering/passes/lighting_passes.h"

#include "rendering/clusters.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/formats.h"

#include <cmath>
#include <cstring>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::uint32_t groups(const std::uint32_t extent) noexcept
        {
            return (extent + 7) / 8;
        }

        constexpr std::uint32_t cluster_groups = (gpu::cluster_count + 63) / 64;

        gpu::float4x4 to_gpu(const Eigen::Matrix4f& matrix) noexcept
        {
            gpu::float4x4 result{};
            std::memcpy(&result, matrix.data(), sizeof(result)); // both column-major
            return result;
        }

        // clusters.hlsl's light_cull draw constants
        struct light_cull_constants
        {
            std::uint32_t bounds;
            std::uint32_t lights;
            std::uint32_t light_count;
            std::uint32_t depth_mips;
            std::uint32_t depth_mip_count;
            std::uint32_t cluster_lights;
            std::uint32_t light_indices;
            std::uint32_t keep_front;
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t tile_width;
            std::uint32_t tile_height;
        };

        struct cluster_build_data
        {
            graphics::buffer bounds;
        };

        struct light_cull_data
        {
            graphics::buffer bounds;
            graphics::buffer lights;
            graphics::texture depth_mips;
            graphics::buffer cluster_lights;
            graphics::buffer light_indices;
        };

        struct deferred_lighting_data
        {
            graphics::texture albedo;
            graphics::texture normal;
            graphics::texture material;
            graphics::texture emissive;
            graphics::texture depth;
            graphics::texture linear_depth;
            graphics::buffer lights;
            graphics::buffer cluster_lights;
            graphics::buffer light_indices;
            graphics::buffer irradiance;
            graphics::texture prefiltered;
            graphics::texture brdf_lut;
            graphics::texture shadow_map;
            graphics::texture ambient_occlusion;
            graphics::buffer exposure;
            graphics::texture hdr_color;
            graphics::texture cascade_index;
        };

        struct sky_data
        {
            graphics::texture hdr_color;
            graphics::texture depth;
            graphics::buffer exposure;
        };
    } // namespace

    float manual_exposure(const float ev100) noexcept
    {
        return 1.0f / (1.2f * std::exp2(ev100));
    }

    light_clusters add_light_clusters(graphics::render_graph& graph, const cluster_inputs& inputs)
    {
        const rhi::access_state common = rhi::access_states::common;
        graphics::buffer bounds = graph.import_buffer("cluster_bounds", inputs.bounds, common, common);
        const graphics::texture_extent tile = cluster_tile_size(inputs.size);

        if (inputs.rebuild)
        {
            gpu::cluster_build_constants constants{};
            constants.projection_inverse = to_gpu(inputs.projection_inverse);
            constants.size = { inputs.size.width, inputs.size.height };
            constants.tile_size = { tile.width, tile.height };
            bounds =
                graph
                    .add_compute_pass<cluster_build_data>(
                        "cluster_build",
                        [&](graphics::render_pass_builder& builder, cluster_build_data& data)
                        { data.bounds = builder.write(bounds, graphics::buffer_write::uav); },
                        [pipeline = inputs.build, constants](
                            const cluster_build_data& data,
                            const graphics::resource_bag& resources,
                            graphics::compute_pass_context& context)
                        {
                            gpu::cluster_build_constants bound = constants;
                            bound.bounds = resources.uav(data.bounds).value;
                            rhi::compute_recorder& recorder = context.recorder();
                            recorder.set_pipeline(pipeline);
                            graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(bound));
                            recorder.dispatch(cluster_groups, 1, 1);
                        },
                        graphics::compute_queue_hint::async_compute)
                    .bounds;
        }

        const light_cull_data& culled = graph.add_compute_pass<light_cull_data>(
            "light_cull",
            [&](graphics::render_pass_builder& builder, light_cull_data& data)
            {
                data.bounds = builder.read(bounds, graphics::buffer_read::srv_compute);
                data.lights = builder.read(inputs.frame.lights, graphics::buffer_read::srv_compute);
                data.depth_mips = builder.read(inputs.depth_mips, graphics::texture_read::srv_compute);
                data.cluster_lights = builder.write(
                    builder.create_buffer(
                        "cluster_lights",
                        { .size = std::uint64_t{ gpu::cluster_count } * sizeof(gpu::cluster_lights),
                          .stride = sizeof(gpu::cluster_lights) }),
                    graphics::buffer_write::uav);
                data.light_indices = builder.write(
                    builder.create_buffer("light_indices", { .size = cluster_light_index_count * 4, .stride = 4 }),
                    graphics::buffer_write::uav);
            },
            [pipeline = inputs.cull,
             lights_srv = inputs.frame.lights_srv,
             light_count = inputs.frame.light_count,
             mips = inputs.depth_mip_count,
             size = inputs.size,
             tile,
             keep_front = inputs.keep_front,
             view_constants = inputs.view_constants](
                const light_cull_data& data,
                const graphics::resource_bag& resources,
                graphics::compute_pass_context& context)
            {
                const light_cull_constants constants{ .bounds = resources.srv(data.bounds).value,
                                                      .lights = lights_srv.value,
                                                      .light_count = light_count,
                                                      .depth_mips = resources.srv(data.depth_mips).value,
                                                      .depth_mip_count = mips,
                                                      .cluster_lights = resources.uav(data.cluster_lights).value,
                                                      .light_indices = resources.uav(data.light_indices).value,
                                                      .keep_front = keep_front ? 1u : 0u,
                                                      .width = size.width,
                                                      .height = size.height,
                                                      .tile_width = tile.width,
                                                      .tile_height = tile.height };
                rhi::compute_recorder& recorder = context.recorder();
                recorder.set_pipeline(pipeline);
                graphics::binding_layout::push_draw_constants(recorder, constants);
                graphics::binding_layout::set_view_constants(recorder, context.allocate_constants(view_constants));
                recorder.dispatch(cluster_groups, 1, 1);
            },
            graphics::compute_queue_hint::async_compute);
        return { .bounds = culled.bounds, .lights = culled.cluster_lights, .indices = culled.light_indices };
    }

    deferred_lighting_outputs
        add_deferred_lighting(graphics::render_graph& graph, const deferred_lighting_inputs& inputs)
    {
        const graphics::texture_extent tile = cluster_tile_size(
            { static_cast<std::uint32_t>(inputs.view_constants.size.x),
              static_cast<std::uint32_t>(inputs.view_constants.size.y) });
        const deferred_lighting_data& lit = graph.add_compute_pass<deferred_lighting_data>(
            "deferred_lighting",
            [&](graphics::render_pass_builder& builder, deferred_lighting_data& data)
            {
                const auto srv = graphics::texture_read::srv_compute;
                data.albedo = builder.read(inputs.gbuffer.albedo, srv);
                data.normal = builder.read(inputs.gbuffer.normal, srv);
                data.material = builder.read(inputs.gbuffer.material, srv);
                data.emissive = builder.read(inputs.gbuffer.emissive, srv);
                data.depth = builder.read(inputs.depth, srv);
                data.linear_depth = builder.read(inputs.linear_depth, srv);
                data.lights = builder.read(inputs.frame.lights, graphics::buffer_read::srv_compute);
                data.cluster_lights = builder.read(inputs.clusters.lights, graphics::buffer_read::srv_compute);
                data.light_indices = builder.read(inputs.clusters.indices, graphics::buffer_read::srv_compute);
                data.irradiance = builder.read(inputs.environment.irradiance, graphics::buffer_read::srv_compute);
                data.exposure = builder.read(inputs.exposure, graphics::buffer_read::srv_compute);
                data.prefiltered = builder.read(inputs.environment.prefiltered, srv);
                data.brdf_lut = builder.read(inputs.environment.brdf_lut, srv);
                if (inputs.shadow_map.valid())
                {
                    data.shadow_map = builder.read(inputs.shadow_map, srv);
                }
                if (inputs.ambient_occlusion.valid())
                {
                    data.ambient_occlusion = builder.read(inputs.ambient_occlusion, srv);
                }
                data.hdr_color = builder.write(
                    builder.create_texture(
                        "hdr_color", { .size = graphics::view_relative(1.0f), .format = rhi::format::rgba16_float }),
                    graphics::texture_write::uav);
                if (inputs.cascade_index)
                {
                    data.cascade_index = builder.write(
                        builder.create_texture(
                            "cascade_index", { .size = graphics::view_relative(1.0f), .format = rhi::format::r8_uint }),
                        graphics::texture_write::uav);
                }
            },
            [pipeline = inputs.pipeline,
             lights_srv = inputs.frame.lights_srv,
             sun = inputs.sun,
             ambient_intensity = inputs.ambient_intensity,
             tile,
             shadow = inputs.shadow,
             ao_scale = inputs.ambient_occlusion_scale,
             view_constants = inputs.view_constants](
                const deferred_lighting_data& data,
                const graphics::resource_bag& resources,
                graphics::compute_pass_context& context)
            {
                const std::uint32_t width = resources.width(data.hdr_color);
                const std::uint32_t height = resources.height(data.hdr_color);
                gpu::deferred_lighting_constants constants{};
                constants.albedo = resources.srv(data.albedo).value;
                constants.normal = resources.srv(data.normal).value;
                constants.material = resources.srv(data.material).value;
                constants.emissive = resources.srv(data.emissive).value;
                constants.depth = resources.srv(data.depth).value;
                constants.linear_depth = resources.srv(data.linear_depth).value;
                constants.hdr_color = resources.uav(data.hdr_color).value;
                constants.lights = lights_srv.value;
                constants.cluster_lights = resources.srv(data.cluster_lights).value;
                constants.light_indices = resources.srv(data.light_indices).value;
                constants.sun = sun;
                constants.ambient_occlusion =
                    data.ambient_occlusion.valid() ? resources.srv(data.ambient_occlusion).value : 0;
                constants.ambient_occlusion_scale = ao_scale;
                constants.irradiance = resources.srv(data.irradiance).value;
                constants.prefiltered = resources.srv(data.prefiltered).value;
                constants.brdf_lut = resources.srv(data.brdf_lut).value;
                constants.cascade_index = data.cascade_index.valid() ? resources.uav(data.cascade_index).value : 0;
                constants.size = { width, height };
                constants.tile_size = { tile.width, tile.height };
                constants.exposure_buffer = resources.srv(data.exposure).value;
                constants.ambient_intensity = ambient_intensity;
                constants.shadow = shadow;
                constants.shadow.shadow_map = data.shadow_map.valid() ? resources.srv(data.shadow_map).value : 0;

                rhi::compute_recorder& recorder = context.recorder();
                recorder.set_pipeline(pipeline);
                graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                graphics::binding_layout::set_view_constants(recorder, context.allocate_constants(view_constants));
                recorder.dispatch(groups(width), groups(height), 1);
            });
        return { .hdr_color = lit.hdr_color, .cascade_index = lit.cascade_index };
    }

    rhi::graphics_pipeline_state sky_pipeline_state() noexcept
    {
        rhi::graphics_pipeline_state state;
        state.rasterizer.cull = rhi::cull_mode::none;
        state.depth = { .test = true, .write = false, .compare = rhi::comparison::equal };
        state.render_target_count = 1;
        state.render_target_formats[0] = rhi::format::rgba16_float;
        state.depth_format = rhi::format::d32_float;
        state.topology = rhi::primitive_topology::triangle_list;
        return state;
    }

    sky_outputs add_sky_pass(graphics::render_graph& graph, const sky_inputs& inputs)
    {
        const sky_data& written = graph.add_graphics_pass<sky_data>(
            "sky",
            [&](graphics::render_pass_builder& builder, sky_data& data)
            {
                data.hdr_color = builder.write(inputs.hdr_color, graphics::texture_write::render_target);
                // Written although the PSO doesn't: the RHI's depth views are writable (scene_range_pass.h)
                data.depth = builder.write(inputs.depth, graphics::texture_write::depth_write);
                data.exposure = builder.read(inputs.exposure, graphics::buffer_read::srv_pixel);
            },
            [pipeline = inputs.pipeline, sky = inputs.sky, view_constants = inputs.view_constants](
                const sky_data& data, const graphics::resource_bag& resources, graphics::graphics_pass_context& context)
            {
                const gpu::sky_pass_constants constants{ .sky = sky,
                                                         .exposure_buffer = resources.srv(data.exposure).value };
                const std::uint32_t width = resources.width(data.hdr_color);
                const std::uint32_t height = resources.height(data.hdr_color);
                const rhi::texture_view target = resources.render_target_view(data.hdr_color);
                rhi::graphics_recorder& recorder = context.recorder();
                recorder.set_render_targets(std::span{ &target, 1 }, resources.depth_stencil_view(data.depth));
                recorder.set_viewport(
                    { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f });
                recorder.set_scissor({ 0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height) });
                recorder.set_pipeline(pipeline);
                graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                graphics::binding_layout::set_view_constants(recorder, context.allocate_constants(view_constants));
                recorder.draw(3);
            });
        return { .hdr_color = written.hdr_color, .depth = written.depth };
    }
} // namespace avis::visualizer::rendering
