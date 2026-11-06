#include "rendering/passes/depth_passes.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"
#include "avis/rhi/formats.h"

#include <algorithm>
#include <bit>
#include <format>

namespace avis::visualizer::rendering
{
    namespace
    {
        // linearize_depth.hlsl's draw constants
        struct linearize_constants
        {
            std::uint32_t depth;
            std::uint32_t linear_depth;
            std::uint32_t depth_mips;
            std::uint32_t padding0;
            std::uint32_t width;
            std::uint32_t height;
        };

        // depth_mips.hlsl's draw constants
        struct depth_mip_constants
        {
            std::uint32_t source;
            std::uint32_t destination;
            std::uint32_t source_mip;
            std::uint32_t padding0;
            std::uint32_t source_width;
            std::uint32_t source_height;
            std::uint32_t destination_width;
            std::uint32_t destination_height;
        };

        struct linearize_data
        {
            graphics::texture depth;
            graphics::texture linear_depth;
            graphics::texture depth_mips;
        };

        struct depth_mip_data
        {
            graphics::texture depth_mips;
            std::uint32_t destination_mip = 1;
        };

        constexpr std::uint32_t mip_extent(const std::uint32_t extent, const std::uint32_t mip) noexcept
        {
            return std::max(extent >> mip, 1u);
        }

        constexpr graphics::subresource_range one_mip(const std::uint32_t mip) noexcept
        {
            return { .first_mip = static_cast<std::uint16_t>(mip), .mip_count = 1 };
        }

        constexpr std::uint32_t groups(const std::uint32_t extent) noexcept
        {
            return (extent + 7) / 8;
        }
    } // namespace

    std::uint32_t depth_mip_levels(const std::uint32_t width, const std::uint32_t height) noexcept
    {
        const std::uint32_t full = std::bit_width(std::max({ width, height, 1u }));
        return std::min(depth_mip_count, full);
    }

    reduction_span depth_mip_span(const std::uint32_t texel, const std::uint32_t source_extent) noexcept
    {
        const std::uint32_t destination_extent = std::max(source_extent / 2, 1u);
        reduction_span span{ .first = texel * 2, .last = std::min(texel * 2 + 1, source_extent - 1) };
        if (texel == destination_extent - 1)
        {
            span.last = source_extent - 1; // an odd extent's last texel
        }
        return span;
    }

    graphics::texture add_depth_prepass(graphics::render_graph& graph, const scene_pass_inputs& inputs)
    {
        scene_attachments attachments;
        graphics::texture_description description{ .size = graphics::view_relative(1.0f),
                                                   .format = rhi::format::d32_float };
        description.clear_value = rhi::clear_value{ .depth = 0.0f }; // reverse-Z: the far plane
        attachments.depth = scene_attachment{ .name = "depth", .description = description, .clear = true };

        const scene_range_pass pass{
            .name = "depth_prepass",
            .kind = pass_kind::depth,
            .range = prefix_range(key_prefix(key_group{ .viewport = viewport::camera() })),
        };
        return add_scene_range_pass(graph, pass, inputs, attachments).depth;
    }

    linearized_depth add_linearize_depth(
        graphics::render_graph& graph,
        const graphics::texture depth,
        const linearize_pipelines& pipelines,
        const gpu::view_constants& view_constants,
        const graphics::texture_extent size)
    {
        const std::uint32_t mips = depth_mip_levels(size.width, size.height);
        const linearize_data& linearized = graph.add_compute_pass<linearize_data>(
            "linearize_depth",
            [&](graphics::render_pass_builder& builder, linearize_data& data)
            {
                data.depth = builder.read(depth, graphics::texture_read::srv_compute);
                data.linear_depth = builder.write(
                    builder.create_texture(
                        "linear_depth", { .size = graphics::view_relative(1.0f), .format = rhi::format::r32_float }),
                    graphics::texture_write::uav);
                // The whole chain counts as written: mip 0 here, the others by the passes below before anyone reads
                // them, so the graph needs no clear of a fresh aliased texture
                data.depth_mips = builder.write(
                    builder.create_texture(
                        "depth_mips",
                        { .size = graphics::view_relative(1.0f),
                          .mip_levels = mips,
                          .format = rhi::format::rg32_float }),
                    graphics::texture_write::uav);
            },
            [pipeline = pipelines.linearize, view_constants](
                const linearize_data& data,
                const graphics::resource_bag& resources,
                graphics::compute_pass_context& context)
            {
                const linearize_constants constants{ .depth = resources.srv(data.depth).value,
                                                     .linear_depth = resources.uav(data.linear_depth).value,
                                                     .depth_mips = resources.uav(data.depth_mips, 0).value,
                                                     .padding0 = 0,
                                                     .width = resources.width(data.depth),
                                                     .height = resources.height(data.depth) };
                rhi::compute_recorder& recorder = context.recorder();
                recorder.set_pipeline(pipeline);
                graphics::binding_layout::push_draw_constants(recorder, constants);
                graphics::binding_layout::set_view_constants(recorder, context.allocate_constants(view_constants));
                recorder.dispatch(groups(constants.width), groups(constants.height), 1);
            });

        graphics::texture chain = linearized.depth_mips;
        for (std::uint32_t mip = 1; mip < mips; ++mip)
        {
            chain = graph
                        .add_compute_pass<depth_mip_data>(
                            std::format("depth_mips_{}", mip),
                            [&](graphics::render_pass_builder& builder, depth_mip_data& data)
                            {
                                builder.read(chain, graphics::texture_read::srv_compute, one_mip(mip - 1));
                                data.depth_mips = builder.write(chain, graphics::texture_write::uav, one_mip(mip));
                                data.destination_mip = mip;
                            },
                            [pipeline = pipelines.reduce, mip, size](
                                const depth_mip_data& data,
                                const graphics::resource_bag& resources,
                                graphics::compute_pass_context& context)
                            {
                                const depth_mip_constants constants{
                                    .source = resources.srv(data.depth_mips).value,
                                    .destination = resources.uav(data.depth_mips, mip).value,
                                    .source_mip = mip - 1,
                                    .padding0 = 0,
                                    .source_width = mip_extent(size.width, mip - 1),
                                    .source_height = mip_extent(size.height, mip - 1),
                                    .destination_width = mip_extent(size.width, mip),
                                    .destination_height = mip_extent(size.height, mip),
                                };
                                rhi::compute_recorder& recorder = context.recorder();
                                recorder.set_pipeline(pipeline);
                                graphics::binding_layout::push_draw_constants(recorder, constants);
                                recorder.dispatch(
                                    groups(constants.destination_width), groups(constants.destination_height), 1);
                            })
                        .depth_mips;
        }
        return { .linear_depth = linearized.linear_depth, .depth_mips = chain };
    }
} // namespace avis::visualizer::rendering
