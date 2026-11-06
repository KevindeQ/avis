#include "rendering/passes/taa_pass.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_pass_builder.h"
#include "avis/graphics/resource_bag.h"

#include <cstdint>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::uint32_t groups(const std::uint32_t extent) noexcept
        {
            return (extent + 7) / 8;
        }

        struct taa_data
        {
            graphics::texture hdr_color;
            graphics::texture velocity;
            graphics::texture linear_depth;
            graphics::texture reactive_mask;
            graphics::texture previous_history;
            graphics::texture previous_depth_history;
            graphics::texture output;
            graphics::texture depth_output;
        };
    } // namespace

    taa_outputs add_taa_resolve(graphics::render_graph& graph, const taa_inputs& inputs)
    {
        const taa_data& written = graph.add_compute_pass<taa_data>(
            "taa_resolve",
            [&](graphics::render_pass_builder& builder, taa_data& data)
            {
                data.hdr_color = builder.read(inputs.hdr_color, graphics::texture_read::srv_compute);
                data.linear_depth = builder.read(inputs.linear_depth, graphics::texture_read::srv_compute);
                if (inputs.has_history)
                {
                    data.velocity = builder.read(inputs.velocity, graphics::texture_read::srv_compute);
                    data.reactive_mask = builder.read(inputs.reactive_mask, graphics::texture_read::srv_compute);
                    data.previous_history = builder.read(inputs.previous_history, graphics::texture_read::srv_compute);
                    data.previous_depth_history =
                        builder.read(inputs.previous_depth_history, graphics::texture_read::srv_compute);
                }
                data.output = builder.write(inputs.history, graphics::texture_write::uav);
                data.depth_output = builder.write(inputs.depth_history, graphics::texture_write::uav);
            },
            [pipeline = inputs.pipeline,
             has_history = inputs.has_history,
             exposure_scale = inputs.history_exposure_scale,
             view_constants = inputs.view_constants](
                const taa_data& data, const graphics::resource_bag& resources, graphics::compute_pass_context& context)
            {
                gpu::taa_constants constants{};
                constants.hdr_color = resources.srv(data.hdr_color).value;
                constants.linear_depth = resources.srv(data.linear_depth).value;
                if (has_history)
                {
                    constants.velocity = resources.srv(data.velocity).value;
                    constants.reactive_mask = resources.srv(data.reactive_mask).value;
                    constants.history = resources.srv(data.previous_history).value;
                    constants.depth_history = resources.srv(data.previous_depth_history).value;
                }
                constants.output = resources.uav(data.output).value;
                constants.depth_output = resources.uav(data.depth_output).value;
                constants.has_history = has_history ? 1u : 0u;
                constants.size = { resources.width(data.output), resources.height(data.output) };
                constants.blend = taa_blend;
                constants.gamma = taa_gamma;
                constants.history_exposure_scale = exposure_scale;
                constants.disocclusion_tolerance = taa_disocclusion_tolerance;

                rhi::compute_recorder& recorder = context.recorder();
                recorder.set_pipeline(pipeline);
                graphics::binding_layout::set_pass_constants(recorder, context.allocate_constants(constants));
                graphics::binding_layout::set_view_constants(recorder, context.allocate_constants(view_constants));
                recorder.dispatch(groups(constants.size.x), groups(constants.size.y), 1);
            });
        return { .output = written.output, .depth_history = written.depth_output };
    }
} // namespace avis::visualizer::rendering
