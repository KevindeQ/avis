#ifndef AVIS_RHI_D3D12_CONVERSIONS_TABLE_H
#define AVIS_RHI_D3D12_CONVERSIONS_TABLE_H

// Lookup tables for the conversions of dense enums: one row per enumerator, checked at compile time

#include <array>
#include <cstddef>
#include <utility>

namespace avis::rhi::d3d12::detail
{
    // True when row i of the table names enumerator i, so the table can be indexed by the enum value
    template<class Source, class Target, std::size_t Size>
    constexpr bool rows_in_enum_order(const std::array<std::pair<Source, Target>, Size>& table) noexcept
    {
        for (std::size_t index = 0; index < Size; ++index)
        {
            if (static_cast<std::size_t>(table[index].first) != index)
            {
                return false;
            }
        }
        return true;
    }

    // The target of a value, or fallback for a value outside the table
    template<class Source, class Target, std::size_t Size>
    constexpr Target look_up(
        const std::array<std::pair<Source, Target>, Size>& table, const Source value, const Target fallback) noexcept
    {
        const auto index = static_cast<std::size_t>(value);
        return index < Size ? table[index].second : fallback;
    }
} // namespace avis::rhi::d3d12::detail

#endif
