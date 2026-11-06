#ifndef AVIS_VISUALIZER_RENDERING_PASSES_GTAO_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_GTAO_PASS_H

// Ground-truth ambient occlusion (docs/design/11-renderer.md §2.3 passes 5 and 6, §8, docs/tasks/11-renderer.md
// §11.13), both on async compute (gtao.hlsl):
//   gtao                the raw AO ("ao_raw", R8_UNORM) and the depth edges ("ao_edges", R8_UNORM, 2 bits per side)
//                       from depth_mips and gbuffer_normal, at half or full resolution
//   gtao_denoise_<n>    gpu::gtao_denoise_passes edge-aware 3 × 3 filters, the last one into "ao" (R8_UNORM)
// At half resolution the textures are half the view's size, rounded up, and AO texel t is centred on pixel 2t;
// deferred_lighting upsamples them.

#include "rendering/render_settings.h"

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/pipeline.h"

#include <cstdint>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    struct gtao_pipelines
    {
        rhi::compute_pipeline_handle gtao; // gtao.hlsl main
        rhi::compute_pipeline_handle denoise; // gtao.hlsl denoise_main
    };

    struct gtao_inputs
    {
        gtao_pipelines pipelines;
        graphics::texture depth_mips;
        std::uint32_t depth_mip_count = 1;
        graphics::texture normal; // gbuffer_normal
        gpu::view_constants view_constants;
        ambient_occlusion_settings parameters;
    };

    struct gtao_outputs
    {
        graphics::texture raw; // ao_raw
        graphics::texture edges; // ao_edges
        graphics::texture ao; // denoised
        std::uint32_t scale = 1; // pixels per AO texel along each axis
    };

    // The pixels per AO texel along each axis: 2 at half resolution, otherwise 1
    constexpr std::uint32_t gtao_scale(const ambient_occlusion_settings& parameters) noexcept
    {
        return parameters.half_resolution ? 2u : 1u;
    }

    // The AO textures' size for a `view` view
    graphics::texture_extent gtao_extent(const ambient_occlusion_settings& parameters, graphics::texture_extent view);

    gtao_outputs add_gtao(graphics::render_graph& graph, const gtao_inputs& inputs);
} // namespace avis::visualizer::rendering

#endif
