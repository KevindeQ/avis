#ifndef AVIS_VISUALIZER_RENDERING_VIEW_CONSTANTS_H
#define AVIS_VISUALIZER_RENDERING_VIEW_CONSTANTS_H

// A view's constants for one frame (docs/design/11-renderer.md §10.2, docs/tasks/11-renderer.md §11.10): the camera's
// matrices, jittered and not, their inverses, the previous frame's view-projection for motion vectors, the jitter, the
// size, the near plane and the device pixel ratio. The scene passes read them as a root CBV at b2 (view_constants in
// shared_types.hlsli).
//
// The jitter is the Halton(2, 3) sequence over 16 samples, applied to the projection as a sub-pixel offset of clip x
// and y. It is only applied when TAA is on for the view; velocity uses the unjittered matrices, so it doesn't see it.

#include "rendering/view.h"

#include <Eigen/Core>

#include <cstdint>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    // The TAA jitter's sequence length (11 §10.2)
    inline constexpr std::uint32_t jitter_sample_count = 16;

    // The radical inverse of `index` in `base` (the Halton sequence's element `index`), in [0, 1)
    float halton(std::uint32_t index, std::uint32_t base) noexcept;

    // Sample `sequence` of the jitter (wrapping every jitter_sample_count frames), in pixels, each component in
    // [-0.5, 0.5): Halton(2, 3) elements 1 to 16, minus 0.5. Pixel y points down.
    Eigen::Vector2f jitter_pixels(std::uint64_t sequence) noexcept;

    // `projection` with its image moved by `jitter` (pixels, y down) on a `width` × `height` target
    Eigen::Matrix4f jitter_projection(
        const Eigen::Matrix4f& projection, const Eigen::Vector2f& jitter, std::uint32_t width, std::uint32_t height);

    struct view_frame
    {
        const view_description& view;
        Eigen::Matrix4f view_projection_previous; // unjittered; this frame's for a view without a previous frame
        Eigen::Vector2f jitter = Eigen::Vector2f::Zero(); // pixels, y down; zero without TAA
        std::uint32_t frame_index = 0; // the frame's noise index, for the shaders that animate noise
    };

    // The constants of `frame`, with its camera's matrices for the view's aspect ratio
    gpu::view_constants make_view_constants(const view_frame& frame);

    // The camera's unjittered view-projection for the view's aspect ratio: what the next frame's
    // view_projection_previous is
    Eigen::Matrix4f unjittered_view_projection(const view_description& view);
} // namespace avis::visualizer::rendering

#endif
