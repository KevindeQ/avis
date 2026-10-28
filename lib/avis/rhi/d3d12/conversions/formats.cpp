#include "avis/rhi/d3d12/conversions/formats.h"

#include "avis/rhi/d3d12/conversions/table.h"

#include <array>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        constexpr std::array<std::pair<format, DXGI_FORMAT>, 28> formats{ {
            { format::unknown, DXGI_FORMAT_UNKNOWN },
            { format::rgba8_unorm, DXGI_FORMAT_R8G8B8A8_UNORM },
            { format::rgba8_unorm_srgb, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB },
            { format::bgra8_unorm, DXGI_FORMAT_B8G8R8A8_UNORM },
            { format::rgb10a2_unorm, DXGI_FORMAT_R10G10B10A2_UNORM },
            { format::r11g11b10_float, DXGI_FORMAT_R11G11B10_FLOAT },
            { format::rgba16_float, DXGI_FORMAT_R16G16B16A16_FLOAT },
            { format::rg16_float, DXGI_FORMAT_R16G16_FLOAT },
            { format::rg16_unorm, DXGI_FORMAT_R16G16_UNORM },
            { format::rg16_snorm, DXGI_FORMAT_R16G16_SNORM },
            { format::r16_float, DXGI_FORMAT_R16_FLOAT },
            { format::r16_uint, DXGI_FORMAT_R16_UINT },
            { format::r8_unorm, DXGI_FORMAT_R8_UNORM },
            { format::r8_uint, DXGI_FORMAT_R8_UINT },
            { format::r32_float, DXGI_FORMAT_R32_FLOAT },
            { format::r32_uint, DXGI_FORMAT_R32_UINT },
            { format::rg32_float, DXGI_FORMAT_R32G32_FLOAT },
            { format::rgba32_float, DXGI_FORMAT_R32G32B32A32_FLOAT },
            { format::rgba32_uint, DXGI_FORMAT_R32G32B32A32_UINT },
            { format::d32_float, DXGI_FORMAT_D32_FLOAT },
            { format::bc1_unorm, DXGI_FORMAT_BC1_UNORM },
            { format::bc1_unorm_srgb, DXGI_FORMAT_BC1_UNORM_SRGB },
            { format::bc3_unorm, DXGI_FORMAT_BC3_UNORM },
            { format::bc3_unorm_srgb, DXGI_FORMAT_BC3_UNORM_SRGB },
            { format::bc4_unorm, DXGI_FORMAT_BC4_UNORM },
            { format::bc5_unorm, DXGI_FORMAT_BC5_UNORM },
            { format::bc7_unorm, DXGI_FORMAT_BC7_UNORM },
            { format::bc7_unorm_srgb, DXGI_FORMAT_BC7_UNORM_SRGB },
        } };

        static_assert(detail::rows_in_enum_order(formats), "one row per format, in enum order");
        static_assert(formats.back().first == format::bc7_unorm_srgb, "the table covers the last format");
        static_assert(detail::look_up(formats, format::d32_float, DXGI_FORMAT_UNKNOWN) == DXGI_FORMAT_D32_FLOAT);
        static_assert(
            detail::look_up(formats, format::bc1_unorm_srgb, DXGI_FORMAT_UNKNOWN) == DXGI_FORMAT_BC1_UNORM_SRGB);
    } // namespace

    DXGI_FORMAT to_native(const format value) noexcept
    {
        return detail::look_up(formats, value, DXGI_FORMAT_UNKNOWN);
    }

    DXGI_FORMAT to_native_typeless(const format value) noexcept
    {
        return value == format::d32_float ? DXGI_FORMAT_R32_TYPELESS : to_native(value);
    }

    DXGI_FORMAT to_native_depth_view(const format value) noexcept
    {
        return value == format::d32_float ? DXGI_FORMAT_R32_FLOAT : to_native(value);
    }
} // namespace avis::rhi::d3d12
