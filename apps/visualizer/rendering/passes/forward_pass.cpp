#include "rendering/passes/forward_pass.h"

#include "rendering/clusters.h"

#include "avis/rhi/formats.h"

#include <cstring>
#include <vector>

namespace avis::visualizer::rendering
{
    namespace
    {
        // Positions in scene_pass_reads
        enum read_texture : std::size_t
        {
            texture_prefiltered,
            texture_brdf_lut,
            texture_shadow_map,
        };
        enum read_buffer : std::size_t
        {
            buffer_lights,
            buffer_cluster_lights,
            buffer_light_indices,
            buffer_irradiance,
            buffer_exposure,
        };
    } // namespace

    key_range forward_range() noexcept
    {
        return prefix_range(
            key_prefix(key_group{ .viewport = viewport::camera(), .translucency = translucency::blended }),
            key_prefix(key_group{ .viewport = viewport::camera(), .translucency = translucency::subtractive }));
    }

    forward_outputs add_forward_transparent(
        graphics::render_graph& graph, const scene_pass_inputs& inputs, const forward_inputs& forward)
    {
        const pass_attachment_formats formats = attachment_formats(pass_kind::forward);
        scene_attachments attachments;
        attachments.color_count = formats.render_targets.size();
        attachments.colors[0] = scene_attachment{ .texture = forward.hdr_color };
        graphics::texture_description reactive{ .size = graphics::view_relative(1.0f),
                                                .format = formats.render_targets[1] };
        reactive.clear_value = rhi::clear_value{}; // 0: nothing reactive
        attachments.colors[1] = scene_attachment{ .name = "reactive_mask", .description = reactive, .clear = true };
        attachments.depth = scene_attachment{ .texture = forward.depth };

        scene_range_pass pass{
            .name = "forward_transparent",
            .kind = pass_kind::forward,
            .range = forward_range(),
        };
        pass.reads.textures[texture_prefiltered] = forward.environment.prefiltered;
        pass.reads.textures[texture_brdf_lut] = forward.environment.brdf_lut;
        pass.reads.textures[texture_shadow_map] = forward.shadow_map;
        pass.reads.buffers[buffer_lights] = inputs.frame.lights;
        pass.reads.buffers[buffer_cluster_lights] = forward.clusters.lights;
        pass.reads.buffers[buffer_light_indices] = forward.clusters.indices;
        pass.reads.buffers[buffer_irradiance] = forward.environment.irradiance;
        pass.reads.buffers[buffer_exposure] = forward.exposure;

        const graphics::texture_extent tile = cluster_tile_size(
            { static_cast<std::uint32_t>(inputs.view_constants.size.x),
              static_cast<std::uint32_t>(inputs.view_constants.size.y) });
        gpu::forward_lighting_constants lighting{};
        lighting.sun = forward.sun;
        lighting.tile_size = { tile.width, tile.height };
        lighting.ambient_intensity = forward.ambient_intensity;
        lighting.shadow = forward.shadow;
        pass.extension = [lighting](const scene_pass_read_srvs& srvs)
        {
            gpu::forward_lighting_constants bound = lighting;
            bound.lights = srvs.buffers[buffer_lights];
            bound.cluster_lights = srvs.buffers[buffer_cluster_lights];
            bound.light_indices = srvs.buffers[buffer_light_indices];
            bound.irradiance = srvs.buffers[buffer_irradiance];
            bound.exposure_buffer = srvs.buffers[buffer_exposure];
            bound.prefiltered = srvs.textures[texture_prefiltered];
            bound.brdf_lut = srvs.textures[texture_brdf_lut];
            bound.shadow.shadow_map = srvs.textures[texture_shadow_map];
            std::vector<std::byte> bytes(sizeof(bound));
            std::memcpy(bytes.data(), &bound, sizeof(bound));
            return bytes;
        };

        const scene_pass_outputs written = add_scene_range_pass(graph, pass, inputs, attachments);
        return { .hdr_color = written.colors[0], .reactive_mask = written.colors[1], .depth = written.depth };
    }
} // namespace avis::visualizer::rendering
