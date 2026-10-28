#include "avis/rhi/d3d12/conversions/barriers.h"

#include "avis/rhi/d3d12/conversions/resources.h"
#include "avis/rhi/d3d12/conversions/table.h"

#include <array>
#include <cstdint>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        template<class Enum>
        constexpr std::uint32_t bits(const Enum value) noexcept
        {
            return static_cast<std::uint32_t>(value);
        }

        // pipeline_synchronization equals D3D12_BARRIER_SYNC bit for bit (§10)
        static_assert(bits(pipeline_synchronization::none) == D3D12_BARRIER_SYNC_NONE);
        static_assert(bits(pipeline_synchronization::all) == D3D12_BARRIER_SYNC_ALL);
        static_assert(bits(pipeline_synchronization::draw) == D3D12_BARRIER_SYNC_DRAW);
        static_assert(bits(pipeline_synchronization::index_input) == D3D12_BARRIER_SYNC_INDEX_INPUT);
        static_assert(bits(pipeline_synchronization::vertex_shading) == D3D12_BARRIER_SYNC_VERTEX_SHADING);
        static_assert(bits(pipeline_synchronization::pixel_shading) == D3D12_BARRIER_SYNC_PIXEL_SHADING);
        static_assert(bits(pipeline_synchronization::depth_stencil) == D3D12_BARRIER_SYNC_DEPTH_STENCIL);
        static_assert(bits(pipeline_synchronization::render_target) == D3D12_BARRIER_SYNC_RENDER_TARGET);
        static_assert(bits(pipeline_synchronization::compute_shading) == D3D12_BARRIER_SYNC_COMPUTE_SHADING);
        static_assert(bits(pipeline_synchronization::copy) == D3D12_BARRIER_SYNC_COPY);
        static_assert(bits(pipeline_synchronization::execute_indirect) == D3D12_BARRIER_SYNC_EXECUTE_INDIRECT);
        static_assert(
            bits(pipeline_synchronization::clear_unordered_access_view) ==
            D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW);

        // resource_access equals D3D12_BARRIER_ACCESS bit for bit (§10)
        static_assert(bits(resource_access::common) == D3D12_BARRIER_ACCESS_COMMON);
        static_assert(bits(resource_access::vertex_buffer) == D3D12_BARRIER_ACCESS_VERTEX_BUFFER);
        static_assert(bits(resource_access::constant_buffer) == D3D12_BARRIER_ACCESS_CONSTANT_BUFFER);
        static_assert(bits(resource_access::index_buffer) == D3D12_BARRIER_ACCESS_INDEX_BUFFER);
        static_assert(bits(resource_access::render_target) == D3D12_BARRIER_ACCESS_RENDER_TARGET);
        static_assert(bits(resource_access::unordered_access) == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        static_assert(bits(resource_access::depth_write) == D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
        static_assert(bits(resource_access::depth_read) == D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);
        static_assert(bits(resource_access::shader_resource) == D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        static_assert(bits(resource_access::indirect_argument) == D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
        static_assert(bits(resource_access::copy_destination) == D3D12_BARRIER_ACCESS_COPY_DEST);
        static_assert(bits(resource_access::copy_source) == D3D12_BARRIER_ACCESS_COPY_SOURCE);
        static_assert(bits(resource_access::no_access) == static_cast<std::uint32_t>(D3D12_BARRIER_ACCESS_NO_ACCESS));

        constexpr std::array<std::pair<texture_layout, D3D12_BARRIER_LAYOUT>, 15> layouts{ {
            { texture_layout::undefined, D3D12_BARRIER_LAYOUT_UNDEFINED },
            { texture_layout::common, D3D12_BARRIER_LAYOUT_COMMON },
            { texture_layout::shader_resource, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE },
            { texture_layout::unordered_access, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS },
            { texture_layout::render_target, D3D12_BARRIER_LAYOUT_RENDER_TARGET },
            { texture_layout::depth_read, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ },
            { texture_layout::depth_write, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE },
            { texture_layout::copy_source, D3D12_BARRIER_LAYOUT_COPY_SOURCE },
            { texture_layout::copy_destination, D3D12_BARRIER_LAYOUT_COPY_DEST },
            { texture_layout::direct_queue_common, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON },
            { texture_layout::direct_queue_shader_resource, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE },
            { texture_layout::direct_queue_unordered_access, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS },
            { texture_layout::compute_queue_common, D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON },
            { texture_layout::compute_queue_shader_resource, D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE },
            { texture_layout::compute_queue_unordered_access, D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS },
        } };

        static_assert(detail::rows_in_enum_order(layouts), "one row per texture layout, in enum order");
        static_assert(
            layouts.back().first == texture_layout::compute_queue_unordered_access,
            "the table covers the last texture layout");

        constexpr std::uint32_t all = 0xFFFFFFFF;
    } // namespace

    D3D12_BARRIER_SYNC to_native(const avis::flags<pipeline_synchronization> value) noexcept
    {
        return static_cast<D3D12_BARRIER_SYNC>(value.underlying_value());
    }

    D3D12_BARRIER_ACCESS to_native(const avis::flags<resource_access> value) noexcept
    {
        return static_cast<D3D12_BARRIER_ACCESS>(value.underlying_value());
    }

    D3D12_BARRIER_LAYOUT to_native(const texture_layout value) noexcept
    {
        return detail::look_up(layouts, value, D3D12_BARRIER_LAYOUT_UNDEFINED);
    }

    D3D12_BARRIER_SUBRESOURCE_RANGE
        to_native(const subresource_range range, const texture_description& texture) noexcept
    {
        // Every subresource: IndexOrFirstMipLevel = 0xFFFFFFFF with NumMipLevels = 0
        if (range.first_mip == 0 && range.mip_count == subresource_range::all && range.first_layer == 0 &&
            range.layer_count == subresource_range::all)
        {
            return { .IndexOrFirstMipLevel = all, .NumMipLevels = 0 };
        }

        const std::uint32_t mips = mip_level_count(texture);
        const std::uint32_t layers = array_size(texture);
        const std::uint32_t mip_count =
            range.mip_count == subresource_range::all ? mips - range.first_mip : range.mip_count;
        const std::uint32_t layer_count =
            range.layer_count == subresource_range::all ? layers - range.first_layer : range.layer_count;

        return {
            .IndexOrFirstMipLevel = range.first_mip,
            .NumMipLevels = mip_count,
            .FirstArraySlice = range.first_layer,
            .NumArraySlices = layer_count,
            .FirstPlane = 0,
            .NumPlanes = 1,
        };
    }
} // namespace avis::rhi::d3d12
