#ifndef AVIS_GRAPHICS_TEXTURE_H
#define AVIS_GRAPHICS_TEXTURE_H

// Texture descriptions and usages of the render graph (docs/design/10-render-graph.md §3.1, §6.3). The description
// carries no RHI usages: the graph derives them from the declared accesses when it compiles.

#include "avis/graphics/resource.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <optional>

namespace avis::graphics
{
    enum class size_mode : std::uint8_t
    {
        absolute, // width and height in texels
        view_relative // scale × the view's size, for example 0.5 for half resolution
    };

    struct texture_extent
    {
        std::uint32_t width = 1;
        std::uint32_t height = 1;

        friend constexpr bool operator==(texture_extent, texture_extent) noexcept = default;
    };

    // Built with absolute() or view_relative()
    struct texture_size
    {
        size_mode mode = size_mode::absolute;
        std::uint32_t width = 1; // absolute only
        std::uint32_t height = 1; // absolute only
        float scale = 1.0f; // view_relative only

        // The size in texels for a view of view_width × view_height. Relative sizes round up and are never 0.
        constexpr texture_extent resolve(const std::uint32_t view_width, const std::uint32_t view_height) const noexcept
        {
            if (mode == size_mode::absolute)
            {
                return { width, height };
            }
            return { scale_up(view_width, scale), scale_up(view_height, scale) };
        }

        friend constexpr bool operator==(const texture_size&, const texture_size&) noexcept = default;

    private:
        static constexpr std::uint32_t scale_up(const std::uint32_t extent, const float factor) noexcept
        {
            const double scaled = static_cast<double>(extent) * static_cast<double>(factor);
            if (!(scaled > 1.0)) // also catches NaN
            {
                return 1;
            }
            if (scaled >= 4294967295.0)
            {
                return 0xFFFF'FFFFu;
            }
            const auto truncated = static_cast<std::uint32_t>(scaled);
            return static_cast<double>(truncated) < scaled ? truncated + 1 : truncated;
        }
    };

    constexpr texture_size absolute(const std::uint32_t width, const std::uint32_t height) noexcept
    {
        return { size_mode::absolute, width, height, 1.0f };
    }

    constexpr texture_size view_relative(const float scale) noexcept
    {
        return { size_mode::view_relative, 1, 1, scale };
    }

    // Replaces the prototype's texture_descriptor
    struct texture_description
    {
        texture_size size = absolute(1, 1);
        std::uint32_t depth_or_layers = 1; // 3D: depth; arrays: layers; cube: number of cubes
        std::uint32_t mip_levels = 1; // 0 = full chain
        rhi::format format = rhi::format::rgba8_unorm;
        rhi::texture_dimension dimension = rhi::texture_dimension::texture_2d;
        std::optional<rhi::clear_value> clear_value; // the optimised clear of render and depth targets
    };

    enum class texture_read : std::uint8_t
    {
        srv_pixel,
        srv_compute,
        depth_read,
        copy_source
    };

    enum class texture_write : std::uint8_t
    {
        render_target,
        depth_write,
        uav,
        copy_destination
    };

    // How a texture write treats the content it doesn't overwrite
    enum class write_flags : std::uint8_t
    {
        none,
        // The pass writes only part of the texture and relies on the rest being cleared: the executor clears it
        // before the pass. Required for a partial first write of an aliased texture (10 §6.4 step 5).
        needs_clear
    };

    // The access state of a usage (10 §6.3). UAVs and depth reads synchronise with the shading stage of the pass's
    // type; copies use the common layout on the copy queue.
    constexpr rhi::access_state
        usage_access_state(const texture_read usage, const pass_type pass, const rhi::queue_type queue) noexcept
    {
        using rhi::pipeline_synchronization;
        using rhi::resource_access;
        using rhi::texture_layout;

        switch (usage)
        {
        case texture_read::srv_pixel:
            return { pipeline_synchronization::pixel_shading,
                     resource_access::shader_resource,
                     texture_layout::shader_resource };
        case texture_read::srv_compute:
            return { pipeline_synchronization::compute_shading,
                     resource_access::shader_resource,
                     texture_layout::shader_resource };
        case texture_read::depth_read:
            return { pipeline_synchronization::depth_stencil |
                         (pass == pass_type::compute ? pipeline_synchronization::compute_shading :
                                                       pipeline_synchronization::pixel_shading),
                     resource_access::depth_read | resource_access::shader_resource,
                     texture_layout::depth_read };
        case texture_read::copy_source:
            return { pipeline_synchronization::copy,
                     resource_access::copy_source,
                     queue == rhi::queue_type::copy ? texture_layout::common : texture_layout::copy_source };
        }
        return {};
    }

    constexpr rhi::access_state
        usage_access_state(const texture_write usage, const pass_type pass, const rhi::queue_type queue) noexcept
    {
        using rhi::pipeline_synchronization;
        using rhi::resource_access;
        using rhi::texture_layout;

        switch (usage)
        {
        case texture_write::render_target:
            return { pipeline_synchronization::render_target,
                     resource_access::render_target,
                     texture_layout::render_target };
        case texture_write::depth_write:
            return { pipeline_synchronization::depth_stencil,
                     resource_access::depth_write,
                     texture_layout::depth_write };
        case texture_write::uav:
            return { pass == pass_type::graphics ? pipeline_synchronization::pixel_shading :
                                                   pipeline_synchronization::compute_shading,
                     resource_access::unordered_access,
                     texture_layout::unordered_access };
        case texture_write::copy_destination:
            return { pipeline_synchronization::copy,
                     resource_access::copy_destination,
                     queue == rhi::queue_type::copy ? texture_layout::common : texture_layout::copy_destination };
        }
        return {};
    }
} // namespace avis::graphics

#endif
