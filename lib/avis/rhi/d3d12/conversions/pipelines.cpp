#include "avis/rhi/d3d12/conversions/pipelines.h"

#include "avis/rhi/d3d12/conversions/table.h"

#include <array>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        constexpr std::array<std::pair<primitive_topology, D3D_PRIMITIVE_TOPOLOGY>, 5> topologies{ {
            { primitive_topology::triangle_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST },
            { primitive_topology::triangle_strip, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP },
            { primitive_topology::line_list, D3D_PRIMITIVE_TOPOLOGY_LINELIST },
            { primitive_topology::line_strip, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP },
            { primitive_topology::point_list, D3D_PRIMITIVE_TOPOLOGY_POINTLIST },
        } };

        static_assert(detail::rows_in_enum_order(topologies), "one row per topology, in enum order");
        static_assert(topologies.back().first == primitive_topology::point_list, "the table covers the last topology");

        constexpr std::array<std::pair<primitive_topology, D3D12_PRIMITIVE_TOPOLOGY_TYPE>, 5> topology_types{ {
            { primitive_topology::triangle_list, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE },
            { primitive_topology::triangle_strip, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE },
            { primitive_topology::line_list, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE },
            { primitive_topology::line_strip, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE },
            { primitive_topology::point_list, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT },
        } };

        static_assert(detail::rows_in_enum_order(topology_types), "one row per topology, in enum order");

        constexpr std::array<std::pair<shader_visibility, D3D12_SHADER_VISIBILITY>, 3> visibilities{ {
            { shader_visibility::all, D3D12_SHADER_VISIBILITY_ALL },
            { shader_visibility::vertex, D3D12_SHADER_VISIBILITY_VERTEX },
            { shader_visibility::pixel, D3D12_SHADER_VISIBILITY_PIXEL },
        } };

        static_assert(detail::rows_in_enum_order(visibilities), "one row per visibility, in enum order");
        static_assert(visibilities.back().first == shader_visibility::pixel, "the table covers the last visibility");
    } // namespace

    D3D_PRIMITIVE_TOPOLOGY to_native(const primitive_topology value) noexcept
    {
        return detail::look_up(topologies, value, D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
    }

    D3D12_SHADER_VISIBILITY to_native(const shader_visibility value) noexcept
    {
        return detail::look_up(visibilities, value, D3D12_SHADER_VISIBILITY_ALL);
    }

    D3D12_PRIMITIVE_TOPOLOGY_TYPE to_native_topology_type(const primitive_topology value) noexcept
    {
        return detail::look_up(topology_types, value, D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED);
    }
} // namespace avis::rhi::d3d12
