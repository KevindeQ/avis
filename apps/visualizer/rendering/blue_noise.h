#ifndef AVIS_VISUALIZER_RENDERING_BLUE_NOISE_H
#define AVIS_VISUALIZER_RENDERING_BLUE_NOISE_H

// The tonemap's blue-noise dither (docs/design/11-renderer.md §10.4, docs/tasks/11-renderer.md §11.16): a tileable
// gpu::blue_noise_size² rank map from Ulichney's void-and-cluster method, generated on the CPU instead of checked in.
// The generation is deterministic (a fixed seed and no platform-dependent distributions), so every run, build and
// machine dithers the same way.

#include <cstdint>
#include <span>

namespace avis::visualizer::rendering
{
    // The rank of each texel, row-major: a permutation of 0 … blue_noise_size² − 1. Thresholding it at any rank gives
    // evenly spread points with no low frequencies. Generated on the first call (tens of milliseconds), then kept.
    std::span<const std::uint16_t> blue_noise_ranks();

    // The texels of the R8_UNORM texture: rank / 16, so each of the 256 levels appears 16 times
    std::span<const std::uint8_t> blue_noise_texels();
} // namespace avis::visualizer::rendering

#endif
