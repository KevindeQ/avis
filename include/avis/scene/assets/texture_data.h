#ifndef AVIS_SCENE_ASSETS_TEXTURE_DATA_H
#define AVIS_SCENE_ASSETS_TEXTURE_DATA_H

// Decoded CPU texture data (docs/design/08-scene-management.md §5.3, §9.4)

#include <cstddef>
#include <cstdint>
#include <vector>

namespace avis::scene
{
    enum class texture_format : std::uint8_t
    {
        rgba8_unorm,
        rgba8_srgb,
        bc4_unorm,
        bc5_unorm,
        bc7_unorm,
        bc7_srgb
    };

    struct texture_data
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        texture_format format = texture_format::rgba8_srgb;
        bool generate_mips = false; // decoded PNG/JPEG: the renderer generates them (§5.3)
        std::vector<std::vector<std::byte>> mips; // tightly packed, mip 0 first

        // Bytes of every stored mip
        std::uint64_t byte_size() const noexcept
        {
            std::uint64_t bytes = 0;
            for (const std::vector<std::byte>& mip : mips)
            {
                bytes += mip.size();
            }
            return bytes;
        }
    };
} // namespace avis::scene

#endif
