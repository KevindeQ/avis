#include "avis/rhi/d3d12/conversions/resources.h"

#include "avis/rhi/d3d12/conversions/formats.h"
#include "avis/rhi/d3d12/conversions/table.h"

#include <algorithm>
#include <array>
#include <bit>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        constexpr std::array<std::pair<memory_type, D3D12_HEAP_TYPE>, 3> heap_types{ {
            { memory_type::gpu_only, D3D12_HEAP_TYPE_DEFAULT },
            { memory_type::upload, D3D12_HEAP_TYPE_UPLOAD },
            { memory_type::readback, D3D12_HEAP_TYPE_READBACK },
        } };

        static_assert(detail::rows_in_enum_order(heap_types), "one row per memory type, in enum order");
        static_assert(heap_types.back().first == memory_type::readback, "the table covers the last memory type");

        constexpr std::uint64_t constant_buffer_alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT; // 256

        D3D12_RESOURCE_DIMENSION dimension(const texture_dimension value) noexcept
        {
            switch (value)
            {
            case texture_dimension::texture_1d:
                return D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            case texture_dimension::texture_3d:
                return D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            case texture_dimension::texture_2d:
            case texture_dimension::cube:
                return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            }
            return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        }
    } // namespace

    D3D12_RESOURCE_DESC1 to_native(const texture_description& description) noexcept
    {
        const auto usage = description.usage;
        const bool depth = usage.count(texture_usage::depth_stencil) != 0;
        const bool sampled = usage.count(texture_usage::shader_resource) != 0;

        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        if (usage.count(texture_usage::unordered_access) != 0)
        {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        if (usage.count(texture_usage::render_target) != 0)
        {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        if (depth)
        {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            if (!sampled)
            {
                flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
            }
        }

        const bool three_dimensional = description.dimension == texture_dimension::texture_3d;
        const bool one_dimensional = description.dimension == texture_dimension::texture_1d;

        return D3D12_RESOURCE_DESC1{
            .Dimension = dimension(description.dimension),
            .Alignment = 0,
            .Width = description.width,
            .Height = one_dimensional ? 1u : description.height,
            .DepthOrArraySize =
                static_cast<UINT16>(three_dimensional ? description.depth_or_layers : array_size(description)),
            .MipLevels = static_cast<UINT16>(description.mip_levels), // 0 = full chain, as in D3D12
            .Format = depth && sampled ? to_native_typeless(description.format) : to_native(description.format),
            .SampleDesc = { .Count = 1, .Quality = 0 },
            .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
            .Flags = flags,
            .SamplerFeedbackMipRegion = {},
        };
    }

    D3D12_RESOURCE_DESC1 to_native(const buffer_description& description) noexcept
    {
        std::uint64_t size = description.size;
        if (description.usage.count(buffer_usage::constant) != 0)
        {
            size = (size + constant_buffer_alignment - 1) / constant_buffer_alignment * constant_buffer_alignment;
        }

        return D3D12_RESOURCE_DESC1{
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Alignment = 0,
            .Width = size,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc = { .Count = 1, .Quality = 0 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            .Flags = description.usage.count(buffer_usage::unordered_access) != 0 ?
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS :
                         D3D12_RESOURCE_FLAG_NONE,
            .SamplerFeedbackMipRegion = {},
        };
    }

    D3D12_HEAP_TYPE to_native(const memory_type value) noexcept
    {
        return detail::look_up(heap_types, value, D3D12_HEAP_TYPE_DEFAULT);
    }

    std::uint32_t mip_level_count(const texture_description& description) noexcept
    {
        if (description.mip_levels != 0)
        {
            return description.mip_levels;
        }
        std::uint32_t largest = std::max(description.width, description.height);
        if (description.dimension == texture_dimension::texture_3d)
        {
            largest = std::max(largest, description.depth_or_layers);
        }
        return static_cast<std::uint32_t>(std::bit_width(std::max(largest, 1u)));
    }

    std::uint32_t array_size(const texture_description& description) noexcept
    {
        switch (description.dimension)
        {
        case texture_dimension::texture_3d:
            return 1;
        case texture_dimension::cube:
            return 6 * description.depth_or_layers;
        case texture_dimension::texture_1d:
        case texture_dimension::texture_2d:
            return description.depth_or_layers;
        }
        return description.depth_or_layers;
    }
} // namespace avis::rhi::d3d12
