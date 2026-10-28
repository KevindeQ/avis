#include "avis/rhi/formats.h"

namespace avis::rhi
{
    std::string_view to_string(const format value) noexcept
    {
        switch (value)
        {
        case format::unknown:
            return "unknown";
        case format::rgba8_unorm:
            return "rgba8_unorm";
        case format::rgba8_unorm_srgb:
            return "rgba8_unorm_srgb";
        case format::bgra8_unorm:
            return "bgra8_unorm";
        case format::rgb10a2_unorm:
            return "rgb10a2_unorm";
        case format::r11g11b10_float:
            return "r11g11b10_float";
        case format::rgba16_float:
            return "rgba16_float";
        case format::rg16_float:
            return "rg16_float";
        case format::rg16_unorm:
            return "rg16_unorm";
        case format::rg16_snorm:
            return "rg16_snorm";
        case format::r16_float:
            return "r16_float";
        case format::r16_uint:
            return "r16_uint";
        case format::r8_unorm:
            return "r8_unorm";
        case format::r8_uint:
            return "r8_uint";
        case format::r32_float:
            return "r32_float";
        case format::r32_uint:
            return "r32_uint";
        case format::rg32_float:
            return "rg32_float";
        case format::rgba32_float:
            return "rgba32_float";
        case format::rgba32_uint:
            return "rgba32_uint";
        case format::d32_float:
            return "d32_float";
        case format::bc1_unorm:
            return "bc1_unorm";
        case format::bc1_unorm_srgb:
            return "bc1_unorm_srgb";
        case format::bc3_unorm:
            return "bc3_unorm";
        case format::bc3_unorm_srgb:
            return "bc3_unorm_srgb";
        case format::bc4_unorm:
            return "bc4_unorm";
        case format::bc5_unorm:
            return "bc5_unorm";
        case format::bc7_unorm:
            return "bc7_unorm";
        case format::bc7_unorm_srgb:
            return "bc7_unorm_srgb";
        }
        return "invalid";
    }
} // namespace avis::rhi
