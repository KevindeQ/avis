#ifndef AVIS_LIB_GRAPHICS_TEXTURE_SUBRESOURCES_H
#define AVIS_LIB_GRAPHICS_TEXTURE_SUBRESOURCES_H

// Private to lib/avis/graphics: how many mips and layers a texture description has, for the upload and readback
// queues' subresource checks.

#include "avis/rhi/resources.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace avis::graphics::detail
{
    inline std::uint32_t mip_count(const rhi::texture_description& description) noexcept
    {
        if (description.mip_levels != 0)
        {
            return description.mip_levels;
        }
        std::uint32_t largest = std::max(description.width, description.height);
        if (description.dimension == rhi::texture_dimension::texture_3d)
        {
            largest = std::max(largest, description.depth_or_layers);
        }
        return static_cast<std::uint32_t>(std::bit_width(std::max(largest, 1u)));
    }

    // Array layers; a cube has six per cube, a 3D texture one
    inline std::uint32_t layer_count(const rhi::texture_description& description) noexcept
    {
        switch (description.dimension)
        {
        case rhi::texture_dimension::texture_3d:
            return 1;
        case rhi::texture_dimension::cube:
            return 6 * description.depth_or_layers;
        case rhi::texture_dimension::texture_1d:
        case rhi::texture_dimension::texture_2d:
            return description.depth_or_layers;
        }
        return description.depth_or_layers;
    }
} // namespace avis::graphics::detail

#endif
