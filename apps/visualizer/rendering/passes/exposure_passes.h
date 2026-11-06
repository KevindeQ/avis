#ifndef AVIS_VISUALIZER_RENDERING_PASSES_EXPOSURE_PASSES_H
#define AVIS_VISUALIZER_RENDERING_PASSES_EXPOSURE_PASSES_H

// Exposure and tonemapping (docs/design/11-renderer.md §2.3 passes 12 and 13, §10.3, §10.4, docs/tasks/11-renderer.md
// §11.16), on the graphics queue's compute (exposure.hlsl, tonemap.hlsl). They work on the view's exposure buffer
// (layout in shared_types.hlsli), which every pass that pre-exposes reads:
//   exposure_reset      one group: the buffer's current and previous exposures become the initial one, its bins 0.
//                       Added first, on a view's first frame, before anything reads the buffer.
//   exposure_histogram  16 × 16 groups, automatic mode only: taa_output's log-luminance histogram, into the buffer
//   exposure_adapt      one group: the next frame's exposure, adapted from the histogram (automatic) or the manual
//                       EV100's (manual); the exposure this frame used becomes the previous one. It clears the bins.
//   tonemap             8 × 8 groups: ldr_color (RGBA8_UNORM) from taa_output rescaled to the adapted exposure,
//                       through ACES fitted or AgX, sRGB-encoded and dithered with blue noise

#include "rendering/render_settings.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/pipeline.h"

namespace avis::visualizer::rendering
{
    // Automatic exposure's fixed parameters (11 §10.3)
    inline constexpr float exposure_key = 0.18f; // the pre-exposed luminance the histogram's average is brought to
    inline constexpr float exposure_brighten_rate = 3.0f; // per second, while the exposure rises
    inline constexpr float exposure_darken_rate = 1.0f; // per second, while it falls
    inline constexpr float exposure_minimum_ev100 = -4.0f; // the range automatic exposure stays in
    inline constexpr float exposure_maximum_ev100 = 20.0f;
    inline constexpr float exposure_maximum_frame_time = 1.0f; // seconds; a longer gap adapts as if it were this

    // exposure_reset, writing `initial` (an exposure, not an EV100); returns the version it wrote
    graphics::buffer add_exposure_reset(
        graphics::render_graph& graph, rhi::compute_pipeline_handle adapt, graphics::buffer exposure, float initial);

    struct exposure_inputs
    {
        rhi::compute_pipeline_handle histogram; // exposure.hlsl histogram_main
        rhi::compute_pipeline_handle adapt; // exposure.hlsl adapt_main
        graphics::texture color; // taa_output
        graphics::buffer exposure; // the view's
        exposure_settings settings;
        float frame_time = 0.0f; // seconds since the view's previous adaptation
    };

    // exposure_histogram (automatic mode) and exposure_adapt; returns the version of the buffer they wrote
    graphics::buffer add_exposure_adaptation(graphics::render_graph& graph, const exposure_inputs& inputs);

    struct tonemap_inputs
    {
        rhi::compute_pipeline_handle pipeline; // tonemap.hlsl main
        graphics::texture color; // taa_output
        graphics::buffer exposure; // the version exposure_adapt wrote
        graphics::texture blue_noise; // R8_UNORM, gpu::blue_noise_size² (blue_noise.h)
        tonemap_operator tonemapper = tonemap_operator::aces;
    };

    // tonemap; returns ldr_color, RGBA8_UNORM, view size
    graphics::texture add_tonemap(graphics::render_graph& graph, const tonemap_inputs& inputs);
} // namespace avis::visualizer::rendering

#endif
