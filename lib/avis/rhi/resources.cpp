#include "avis/rhi/resources.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/conversions/resources.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/render_hardware_interface.h"

namespace avis::rhi
{
    namespace
    {
        bool has(const avis::flags<texture_usage> usage, const texture_usage value) noexcept
        {
            return usage.count(value) != 0;
        }

        bool has(const avis::flags<buffer_usage> usage, const buffer_usage value) noexcept
        {
            return usage.count(value) != 0;
        }
    } // namespace

    resources::resources(render_hardware_interface& hardware_interface) noexcept : state_{ &hardware_interface.state() }
    {}

    result<texture_handle> resources::create_texture(const texture_description& description)
    {
        return state_->resources().create_texture(description);
    }

    result<buffer_handle> resources::create_buffer(const buffer_description& description)
    {
        return state_->resources().create_buffer(description);
    }

    result<sampler_handle> resources::create_sampler(const sampler_description& description)
    {
        return state_->resources().create_sampler(description);
    }

    void resources::destroy(const texture_handle texture) noexcept
    {
        state_->resources().destroy(texture);
    }

    void resources::destroy(const buffer_handle buffer) noexcept
    {
        state_->resources().destroy(buffer);
    }

    void resources::destroy(const sampler_handle sampler) noexcept
    {
        state_->resources().destroy(sampler);
    }

    const texture_description& resources::description(const texture_handle texture) const
    {
        static const texture_description stale{};
        const d3d12::texture_record* const record = state_->resources().find(texture);
        if (!AVIS_RHI_CHECK(record != nullptr, "description: stale or invalid texture handle"))
        {
            return stale;
        }
        return record->description;
    }

    const buffer_description& resources::description(const buffer_handle buffer) const
    {
        static const buffer_description stale{};
        const d3d12::buffer_record* const record = state_->resources().find(buffer);
        if (!AVIS_RHI_CHECK(record != nullptr, "description: stale or invalid buffer handle"))
        {
            return stale;
        }
        return record->description;
    }

    bindless_index resources::srv(const texture_handle texture) const
    {
        const d3d12::texture_record* const record = state_->resources().find(texture);
        if (!AVIS_RHI_CHECK(record != nullptr, "srv: stale or invalid texture handle") ||
            !AVIS_RHI_CHECK(!record->placed, "srv: placed textures have no persistent views; use a descriptor_range") ||
            !AVIS_RHI_CHECK(
                has(record->description.usage, texture_usage::shader_resource),
                "srv: the texture wasn't created with shader_resource usage"))
        {
            return {};
        }
        return record->srv.index();
    }

    bindless_index resources::uav(const texture_handle texture, const std::uint32_t mip) const
    {
        const d3d12::texture_record* const record = state_->resources().find(texture);
        if (!AVIS_RHI_CHECK(record != nullptr, "uav: stale or invalid texture handle") ||
            !AVIS_RHI_CHECK(!record->placed, "uav: placed textures have no persistent views; use a descriptor_range") ||
            !AVIS_RHI_CHECK(
                has(record->description.usage, texture_usage::unordered_access),
                "uav: the texture wasn't created with unordered_access usage") ||
            !AVIS_RHI_CHECK(mip < record->uav.size(), "uav: mip out of range"))
        {
            return {};
        }
        return record->uav[mip].index();
    }

    bindless_index resources::srv(const buffer_handle buffer) const
    {
        const d3d12::buffer_record* const record = state_->resources().find(buffer);
        if (!AVIS_RHI_CHECK(record != nullptr, "srv: stale or invalid buffer handle") ||
            !AVIS_RHI_CHECK(!record->placed, "srv: placed buffers have no persistent views; use a descriptor_range") ||
            !AVIS_RHI_CHECK(
                has(record->description.usage, buffer_usage::shader_resource),
                "srv: the buffer wasn't created with shader_resource usage"))
        {
            return {};
        }
        return record->srv.index();
    }

    bindless_index resources::uav(const buffer_handle buffer) const
    {
        const d3d12::buffer_record* const record = state_->resources().find(buffer);
        if (!AVIS_RHI_CHECK(record != nullptr, "uav: stale or invalid buffer handle") ||
            !AVIS_RHI_CHECK(!record->placed, "uav: placed buffers have no persistent views; use a descriptor_range") ||
            !AVIS_RHI_CHECK(
                has(record->description.usage, buffer_usage::unordered_access),
                "uav: the buffer wasn't created with unordered_access usage"))
        {
            return {};
        }
        return record->uav.index();
    }

    bindless_index resources::sampler_index(const sampler_handle sampler) const
    {
        const d3d12::sampler_record* const record = state_->resources().find(sampler);
        if (!AVIS_RHI_CHECK(record != nullptr, "sampler_index: stale or invalid sampler handle"))
        {
            return {};
        }
        return record->index.index();
    }

    gpu_address resources::buffer_address(const buffer_handle buffer) const
    {
        const d3d12::buffer_record* const record = state_->resources().find(buffer);
        if (!AVIS_RHI_CHECK(record != nullptr, "buffer_address: stale or invalid buffer handle"))
        {
            return {};
        }
        return record->address;
    }

    std::span<std::byte> resources::mapped_bytes(const buffer_handle buffer) const
    {
        const d3d12::buffer_record* const record = state_->resources().find(buffer);
        if (!AVIS_RHI_CHECK(record != nullptr, "mapped_bytes: stale or invalid buffer handle") ||
            !AVIS_RHI_CHECK(
                record->description.memory != memory_type::gpu_only,
                "mapped_bytes: only upload and readback buffers are mapped"))
        {
            return {};
        }
        return record->mapped;
    }

    copy_footprint resources::copyable_footprint(const texture_handle texture, const subresource subresource) const
    {
        const d3d12::texture_record* const record = state_->resources().find(texture);
        if (!AVIS_RHI_CHECK(record != nullptr, "copyable_footprint: stale or invalid texture handle"))
        {
            return {};
        }
        const texture_description& description = record->description;
        const std::uint32_t mips = d3d12::mip_level_count(description);
        const std::uint32_t layers =
            description.dimension == texture_dimension::texture_3d ? 1 : d3d12::array_size(description);
        if (!AVIS_RHI_CHECK(
                subresource.mip < mips && subresource.layer < layers, "copyable_footprint: subresource out of range"))
        {
            return {};
        }

        D3D12_RESOURCE_DESC1 native = d3d12::to_native(description);
        native.MipLevels = static_cast<UINT16>(mips);
        const UINT index = subresource.mip + subresource.layer * mips;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT rows = 0;
        UINT64 row_size = 0;
        UINT64 total = 0;
        state_->device().native().GetCopyableFootprints1(&native, index, 1, 0, &layout, &rows, &row_size, &total);
        return {
            .offset = layout.Offset,
            .width = layout.Footprint.Width,
            .height = layout.Footprint.Height,
            .depth = layout.Footprint.Depth,
            .row_pitch = layout.Footprint.RowPitch,
            .row_count = rows,
            .row_size = row_size,
            .total_size = total,
        };
    }
} // namespace avis::rhi
