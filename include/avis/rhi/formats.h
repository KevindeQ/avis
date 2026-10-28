#ifndef AVIS_RHI_FORMATS_H
#define AVIS_RHI_FORMATS_H

// Texture and view formats (docs/design/09-rhi.md §3.5). The mapping to DXGI_FORMAT is a private table.

#include <cstdint>
#include <string_view>

namespace avis::rhi
{
    enum class format : std::uint8_t
    {
        unknown,
        rgba8_unorm,
        rgba8_unorm_srgb,
        bgra8_unorm,
        rgb10a2_unorm,
        r11g11b10_float,
        rgba16_float,
        rg16_float,
        rg16_unorm,
        rg16_snorm,
        r16_float,
        r16_uint,
        r8_unorm,
        r8_uint,
        r32_float,
        r32_uint,
        rg32_float,
        rgba32_float,
        rgba32_uint,
        d32_float,
        bc1_unorm,
        bc1_unorm_srgb,
        bc3_unorm,
        bc3_unorm_srgb,
        bc4_unorm,
        bc5_unorm,
        bc7_unorm,
        bc7_unorm_srgb
    };

    struct format_traits
    {
        std::uint8_t bytes_per_block = 0; // per texel for uncompressed formats
        std::uint8_t block_width = 1; // 4 for BC formats
        std::uint8_t block_height = 1;
        bool depth = false;
        bool srgb = false;
        bool compressed = false;

        friend constexpr bool operator==(const format_traits&, const format_traits&) noexcept = default;
    };

    constexpr format_traits traits(const format value) noexcept
    {
        constexpr auto texel = [](const std::uint8_t bytes) { return format_traits{ .bytes_per_block = bytes }; };
        constexpr auto texel_srgb = [](const std::uint8_t bytes)
        { return format_traits{ .bytes_per_block = bytes, .srgb = true }; };
        constexpr auto block = [](const std::uint8_t bytes, const bool srgb)
        {
            return format_traits{
                .bytes_per_block = bytes, .block_width = 4, .block_height = 4, .srgb = srgb, .compressed = true
            };
        };

        switch (value)
        {
        case format::unknown:
            return {};
        case format::rgba8_unorm:
            return texel(4);
        case format::rgba8_unorm_srgb:
            return texel_srgb(4);
        case format::bgra8_unorm:
        case format::rgb10a2_unorm:
        case format::r11g11b10_float:
            return texel(4);
        case format::rgba16_float:
            return texel(8);
        case format::rg16_float:
        case format::rg16_unorm:
        case format::rg16_snorm:
            return texel(4);
        case format::r16_float:
        case format::r16_uint:
            return texel(2);
        case format::r8_unorm:
        case format::r8_uint:
            return texel(1);
        case format::r32_float:
        case format::r32_uint:
            return texel(4);
        case format::rg32_float:
            return texel(8);
        case format::rgba32_float:
        case format::rgba32_uint:
            return texel(16);
        case format::d32_float:
            return format_traits{ .bytes_per_block = 4, .depth = true };
        case format::bc1_unorm:
            return block(8, false);
        case format::bc1_unorm_srgb:
            return block(8, true);
        case format::bc3_unorm:
            return block(16, false);
        case format::bc3_unorm_srgb:
            return block(16, true);
        case format::bc4_unorm:
            return block(8, false);
        case format::bc5_unorm:
        case format::bc7_unorm:
            return block(16, false);
        case format::bc7_unorm_srgb:
            return block(16, true);
        }
        return {};
    }

    // The enumerator's name, for example "rgba8_unorm"; "invalid" for a value outside the enum
    std::string_view to_string(format value) noexcept;
} // namespace avis::rhi

#endif
