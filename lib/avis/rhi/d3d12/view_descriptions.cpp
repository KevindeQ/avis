#include "avis/rhi/d3d12/view_descriptions.h"

#include "avis/rhi/d3d12/conversions/formats.h"
#include "avis/rhi/d3d12/conversions/resources.h"

#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        // Resolves `all` against the total
        UINT span_of(const std::uint16_t first, const std::uint16_t count, const std::uint32_t total) noexcept
        {
            if (count == subresource_range::all)
            {
                return first < total ? total - first : 0;
            }
            return count;
        }

        std::pair<UINT, UINT> buffer_elements(const buffer_description& description) noexcept
        {
            if (description.structure_stride > 0)
            {
                return { static_cast<UINT>(description.size / description.structure_stride), description.structure_stride };
            }
            return { static_cast<UINT>(description.size / 4), 0 };
        }
    } // namespace

    D3D12_SHADER_RESOURCE_VIEW_DESC texture_srv(const texture_description& description, const subresource_range range) noexcept
    {
        const bool depth = description.usage.count(texture_usage::depth_stencil) != 0;
        const std::uint32_t total_layers = array_size(description);
        const UINT first_mip = range.first_mip;
        const UINT mips = span_of(range.first_mip, range.mip_count, mip_level_count(description));
        const UINT first_layer = range.first_layer;
        const UINT layers = span_of(range.first_layer, range.layer_count, total_layers);
        const bool every_layer = first_layer == 0 && layers == total_layers;

        D3D12_SHADER_RESOURCE_VIEW_DESC view{
            .Format = depth ? to_native_depth_view(description.format) : to_native(description.format),
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        };
        switch (description.dimension)
        {
        case texture_dimension::texture_1d:
            if (total_layers > 1)
            {
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                view.Texture1DArray = { .MostDetailedMip = first_mip, .MipLevels = mips, .FirstArraySlice = first_layer,
                                        .ArraySize = layers, .ResourceMinLODClamp = 0.0f };
            }
            else
            {
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                view.Texture1D = { .MostDetailedMip = first_mip, .MipLevels = mips, .ResourceMinLODClamp = 0.0f };
            }
            break;
        case texture_dimension::texture_3d:
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            view.Texture3D = { .MostDetailedMip = first_mip, .MipLevels = mips, .ResourceMinLODClamp = 0.0f };
            break;
        case texture_dimension::cube:
            if (every_layer)
            {
                if (description.depth_or_layers > 1)
                {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                    view.TextureCubeArray = { .MostDetailedMip = first_mip, .MipLevels = mips, .First2DArrayFace = 0,
                                              .NumCubes = description.depth_or_layers, .ResourceMinLODClamp = 0.0f };
                }
                else
                {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    view.TextureCube = { .MostDetailedMip = first_mip, .MipLevels = mips, .ResourceMinLODClamp = 0.0f };
                }
                break;
            }
            [[fallthrough]]; // some faces: a 2D array view
        case texture_dimension::texture_2d:
            if (total_layers > 1)
            {
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray = { .MostDetailedMip = first_mip, .MipLevels = mips, .FirstArraySlice = first_layer,
                                        .ArraySize = layers, .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f };
            }
            else
            {
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Texture2D = { .MostDetailedMip = first_mip, .MipLevels = mips, .PlaneSlice = 0,
                                   .ResourceMinLODClamp = 0.0f };
            }
            break;
        }
        return view;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC texture_uav(const texture_description& description, const std::uint32_t mip) noexcept
    {
        const auto layers = static_cast<UINT>(array_size(description));
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{ .Format = to_native(description.format) };
        switch (description.dimension)
        {
        case texture_dimension::texture_1d:
            if (layers > 1)
            {
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                view.Texture1DArray = { .MipSlice = mip, .FirstArraySlice = 0, .ArraySize = layers };
            }
            else
            {
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                view.Texture1D = { .MipSlice = mip };
            }
            break;
        case texture_dimension::texture_2d:
        case texture_dimension::cube:
            if (layers > 1)
            {
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray = { .MipSlice = mip, .FirstArraySlice = 0, .ArraySize = layers, .PlaneSlice = 0 };
            }
            else
            {
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                view.Texture2D = { .MipSlice = mip, .PlaneSlice = 0 };
            }
            break;
        case texture_dimension::texture_3d:
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            view.Texture3D = { .MipSlice = mip, .FirstWSlice = 0, .WSize = static_cast<UINT>(-1) };
            break;
        }
        return view;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC buffer_srv(const buffer_description& description) noexcept
    {
        const auto [elements, stride] = buffer_elements(description);
        const bool raw = stride == 0;
        return {
            .Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN,
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = { .FirstElement = 0,
                        .NumElements = elements,
                        .StructureByteStride = stride,
                        .Flags = raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE },
        };
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC buffer_uav(const buffer_description& description) noexcept
    {
        const auto [elements, stride] = buffer_elements(description);
        const bool raw = stride == 0;
        return {
            .Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN,
            .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
            .Buffer = { .FirstElement = 0,
                        .NumElements = elements,
                        .StructureByteStride = stride,
                        .CounterOffsetInBytes = 0,
                        .Flags = raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE },
        };
    }
} // namespace avis::rhi::d3d12
