#ifndef AVIS_GRAPHICS_BUFFER_H
#define AVIS_GRAPHICS_BUFFER_H

// Buffer descriptions and usages of the render graph (docs/design/10-render-graph.md §3.1, §6.3). The description
// carries no RHI usages: the graph derives them from the declared accesses when it compiles.

#include "avis/graphics/resource.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/types.h"

#include <cstdint>

namespace avis::graphics
{
    struct buffer_description
    {
        std::uint64_t size = 0; // bytes
        std::uint32_t stride = 0; // structured views: bytes per element; 0 for raw or typeless buffers
        bool raw = false; // views are ByteAddressBuffers
    };

    enum class buffer_read : std::uint8_t
    {
        srv_pixel,
        srv_compute,
        indirect_arguments,
        copy_source
    };

    enum class buffer_write : std::uint8_t
    {
        uav,
        copy_destination
    };

    // The access state of a usage (10 §6.3). Buffers have no layout: it stays at its default, which barriers ignore.
    // UAVs synchronise with the shading stage of the pass's type.
    constexpr rhi::access_state usage_access_state(
        const buffer_read usage,
        [[maybe_unused]] const pass_type pass,
        [[maybe_unused]] const rhi::queue_type queue) noexcept
    {
        using rhi::pipeline_synchronization;
        using rhi::resource_access;

        switch (usage)
        {
        case buffer_read::srv_pixel:
            return { pipeline_synchronization::pixel_shading, resource_access::shader_resource };
        case buffer_read::srv_compute:
            return { pipeline_synchronization::compute_shading, resource_access::shader_resource };
        case buffer_read::indirect_arguments:
            return { pipeline_synchronization::execute_indirect, resource_access::indirect_argument };
        case buffer_read::copy_source:
            return { pipeline_synchronization::copy, resource_access::copy_source };
        }
        return {};
    }

    constexpr rhi::access_state usage_access_state(
        const buffer_write usage, const pass_type pass, [[maybe_unused]] const rhi::queue_type queue) noexcept
    {
        using rhi::pipeline_synchronization;
        using rhi::resource_access;

        switch (usage)
        {
        case buffer_write::uav:
            return { pass == pass_type::graphics ? pipeline_synchronization::pixel_shading :
                                                   pipeline_synchronization::compute_shading,
                     resource_access::unordered_access };
        case buffer_write::copy_destination:
            return { pipeline_synchronization::copy, resource_access::copy_destination };
        }
        return {};
    }
} // namespace avis::graphics

#endif
