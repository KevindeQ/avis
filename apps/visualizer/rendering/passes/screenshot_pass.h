#ifndef AVIS_VISUALIZER_RENDERING_PASSES_SCREENSHOT_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_SCREENSHOT_PASS_H

// The passes of a screenshot (docs/design/11-renderer.md §12.1, docs/tasks/11-renderer.md §11.21, screenshot.hlsl).
// A custom screenshot's view, each sub-frame:
//   screenshot_exposure    copy: the source view's exposure into the screenshot view's buffer, before its lighting
//   screenshot_accumulate  compute: hdr_color into the accumulation texture, after the transparents
// and on its last sub-frame:
//   screenshot_resolve     compute: the sub-frames' average, which the tonemap reads in place of taa_output
// Both kinds, on the frame that copies:
//   screenshot_capture     copy: ldr_color into the job's readback buffer

#include "avis/graphics/buffer.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/pipeline.h"

#include <cstdint>

namespace avis::visualizer::rendering
{
    struct screenshot_pipelines
    {
        rhi::compute_pipeline_handle accumulate; // screenshot.hlsl accumulate_main
        rhi::compute_pipeline_handle resolve; // screenshot.hlsl resolve_main
    };

    // screenshot_exposure (copy): the source view's exposure (the version its passes last wrote) into the screenshot
    // view's buffer, its current exposure into both header slots, so the screenshot view pre-exposes with the source's
    // latest exposure and its tonemap rescales by 1. Returns the version it wrote.
    graphics::buffer
        add_screenshot_exposure(graphics::render_graph& graph, graphics::buffer source, graphics::buffer exposure);

    // screenshot_accumulate (compute): hdr_color into the imported accumulation texture; returns the version it wrote
    graphics::texture add_screenshot_accumulate(
        graphics::render_graph& graph,
        const screenshot_pipelines& pipelines,
        graphics::texture hdr_color,
        graphics::texture accumulation,
        graphics::buffer exposure,
        bool first);

    // screenshot_resolve (compute): the average of the sub-frames, pre-exposed, into a new RGBA32F texture of the
    // view's size, which the tonemap reads
    graphics::texture add_screenshot_resolve(
        graphics::render_graph& graph,
        const screenshot_pipelines& pipelines,
        graphics::texture accumulation,
        graphics::buffer exposure,
        std::uint32_t subframes);

    // screenshot_capture (copy): ldr_color into the imported readback buffer
    void add_screenshot_copy(graphics::render_graph& graph, graphics::texture ldr_color, graphics::buffer readback);
} // namespace avis::visualizer::rendering

#endif
