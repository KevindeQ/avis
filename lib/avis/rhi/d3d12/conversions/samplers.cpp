#include "avis/rhi/d3d12/conversions/samplers.h"

#include "avis/rhi/d3d12/conversions/table.h"

#include <array>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        constexpr std::array<std::pair<comparison, D3D12_COMPARISON_FUNC>, 8> comparisons{ {
            { comparison::never, D3D12_COMPARISON_FUNC_NEVER },
            { comparison::less, D3D12_COMPARISON_FUNC_LESS },
            { comparison::equal, D3D12_COMPARISON_FUNC_EQUAL },
            { comparison::less_equal, D3D12_COMPARISON_FUNC_LESS_EQUAL },
            { comparison::greater, D3D12_COMPARISON_FUNC_GREATER },
            { comparison::not_equal, D3D12_COMPARISON_FUNC_NOT_EQUAL },
            { comparison::greater_equal, D3D12_COMPARISON_FUNC_GREATER_EQUAL },
            { comparison::always, D3D12_COMPARISON_FUNC_ALWAYS },
        } };

        static_assert(detail::rows_in_enum_order(comparisons), "one row per comparison, in enum order");
        static_assert(comparisons.back().first == comparison::always, "the table covers the last comparison");

        constexpr std::array<std::pair<address_mode, D3D12_TEXTURE_ADDRESS_MODE>, 4> address_modes{ {
            { address_mode::wrap, D3D12_TEXTURE_ADDRESS_MODE_WRAP },
            { address_mode::clamp, D3D12_TEXTURE_ADDRESS_MODE_CLAMP },
            { address_mode::mirror, D3D12_TEXTURE_ADDRESS_MODE_MIRROR },
            { address_mode::border, D3D12_TEXTURE_ADDRESS_MODE_BORDER },
        } };

        static_assert(detail::rows_in_enum_order(address_modes), "one row per address mode, in enum order");
        static_assert(address_modes.back().first == address_mode::border, "the table covers the last address mode");

        constexpr D3D12_FILTER_TYPE filter_type(const filter value) noexcept
        {
            return value == filter::point ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
        }
    } // namespace

    D3D12_COMPARISON_FUNC to_native(const comparison value) noexcept
    {
        return detail::look_up(comparisons, value, D3D12_COMPARISON_FUNC_NEVER);
    }

    D3D12_TEXTURE_ADDRESS_MODE to_native(const address_mode value) noexcept
    {
        return detail::look_up(address_modes, value, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    }

    D3D12_FILTER to_native_filter(
        const filter minification, const filter magnification, const filter mip, const bool compare) noexcept
    {
        const D3D12_FILTER_REDUCTION_TYPE reduction =
            compare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
        if (minification == filter::anisotropic || magnification == filter::anisotropic || mip == filter::anisotropic)
        {
            return static_cast<D3D12_FILTER>(D3D12_ENCODE_ANISOTROPIC_FILTER(reduction));
        }
        return static_cast<D3D12_FILTER>(D3D12_ENCODE_BASIC_FILTER(
            filter_type(minification), filter_type(magnification), filter_type(mip), reduction));
    }
} // namespace avis::rhi::d3d12
