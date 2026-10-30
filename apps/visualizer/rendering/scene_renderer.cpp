#include "scene_renderer.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/resources.h"

namespace rendering
{
    namespace
    {
        namespace graphics = avis::graphics;
        namespace rhi = avis::rhi;

        graphics::texture_description full_resolution(const rhi::format format)
        {
            return { .size = graphics::view_relative(1.0f), .format = format };
        }
    } // namespace

    void scene_renderer::render(
        graphics::render_graph& graph, avis::scene::scene& current_scene, const graphics::texture_extent view_size)
    {
        graph.reset();
        graph.set_view_size(view_size);
        add_passes(graph, current_scene);
        // Nothing writes an import (the Visualizer's output comes with its frame chain, 12), so every pass is culled
        graph.compile();
    }

    void scene_renderer::add_passes(graphics::render_graph& graph, [[maybe_unused]] avis::scene::scene& current_scene)
    {
        struct depth_renderpass
        {
            graphics::texture depth;
        };

        struct linear_depth_renderpass
        {
            graphics::texture linear_depth;
        };

        struct gbuffer_renderpass
        {
            graphics::texture normal;
            graphics::texture albedo;
            graphics::texture roughness;
        };

        struct ambient_occlusion_renderpass
        {
            graphics::texture ambient_occlusion;
        };

        struct lighting_renderpass
        {
            graphics::texture lighting;
        };

        // Depth prepass. Will draw the scene's meshes (view_entities<mesh, material>) once the pipelines exist.
        const depth_renderpass& depth_pass = graph.add_graphics_pass<depth_renderpass>(
            "depth",
            [&](graphics::render_pass_builder& builder, depth_renderpass& data)
            {
                graphics::texture_description description = full_resolution(rhi::format::d32_float);
                description.clear_value = rhi::clear_value{ .depth = 0.0f }; // reverse-Z: 0 is the far plane
                data.depth =
                    builder.write(builder.create_texture("depth", description), graphics::texture_write::depth_write);
            },
            [](const depth_renderpass&, const graphics::resource_bag&, graphics::graphics_pass_context&) {});

        // Linear depth for ambient occlusion and clustering
        const linear_depth_renderpass& linear_depth_pass = graph.add_compute_pass<linear_depth_renderpass>(
            "linearize_depth",
            [&](graphics::render_pass_builder& builder, linear_depth_renderpass& data)
            {
                builder.read(depth_pass.depth, graphics::texture_read::srv_compute);
                data.linear_depth = builder.write(
                    builder.create_texture("linear_depth", full_resolution(rhi::format::r32_float)),
                    graphics::texture_write::uav);
            },
            [](const linear_depth_renderpass&, const graphics::resource_bag&, graphics::compute_pass_context&) {});

        const gbuffer_renderpass& gbuffer_pass = graph.add_graphics_pass<gbuffer_renderpass>(
            "gbuffer",
            [&](graphics::render_pass_builder& builder, gbuffer_renderpass& data)
            {
                builder.read(depth_pass.depth, graphics::texture_read::depth_read);
                data.normal = builder.write(
                    builder.create_texture("gbuffer.normal", full_resolution(rhi::format::rgba16_float)),
                    graphics::texture_write::render_target);
                data.albedo = builder.write(
                    builder.create_texture("gbuffer.albedo", full_resolution(rhi::format::rgba8_unorm)),
                    graphics::texture_write::render_target);
                data.roughness = builder.write(
                    builder.create_texture("gbuffer.roughness", full_resolution(rhi::format::r8_unorm)),
                    graphics::texture_write::render_target);
            },
            [](const gbuffer_renderpass&, const graphics::resource_bag&, graphics::graphics_pass_context&) {});

        // Half resolution, on the async compute queue alongside the G-buffer
        const ambient_occlusion_renderpass& ambient_occlusion_pass =
            graph.add_compute_pass<ambient_occlusion_renderpass>(
                "ambient_occlusion",
                [&](graphics::render_pass_builder& builder, ambient_occlusion_renderpass& data)
                {
                    builder.read(linear_depth_pass.linear_depth, graphics::texture_read::srv_compute);
                    data.ambient_occlusion = builder.write(
                        builder.create_texture(
                            "ambient_occlusion",
                            { .size = graphics::view_relative(0.5f), .format = rhi::format::r8_unorm }),
                        graphics::texture_write::uav);
                },
                [](const ambient_occlusion_renderpass&,
                   const graphics::resource_bag&,
                   graphics::compute_pass_context&) {},
                graphics::compute_queue_hint::async_compute);

        graph.add_graphics_pass<lighting_renderpass>(
            "lighting",
            [&](graphics::render_pass_builder& builder, lighting_renderpass& data)
            {
                builder.read(depth_pass.depth, graphics::texture_read::srv_pixel);
                builder.read(gbuffer_pass.normal, graphics::texture_read::srv_pixel);
                builder.read(gbuffer_pass.albedo, graphics::texture_read::srv_pixel);
                builder.read(gbuffer_pass.roughness, graphics::texture_read::srv_pixel);
                builder.read(ambient_occlusion_pass.ambient_occlusion, graphics::texture_read::srv_pixel);
                data.lighting = builder.write(
                    builder.create_texture("lighting", full_resolution(rhi::format::rgba16_float)),
                    graphics::texture_write::render_target);
            },
            [](const lighting_renderpass&, const graphics::resource_bag&, graphics::graphics_pass_context&) {});
    }
} // namespace rendering
