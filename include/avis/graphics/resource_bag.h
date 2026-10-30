#ifndef AVIS_GRAPHICS_RESOURCE_BAG_H
#define AVIS_GRAPHICS_RESOURCE_BAG_H

// The execution-time mapping from virtual resources to physical ones (docs/design/10-render-graph.md §3.4). Filled by
// render_graph::materialise; passes see it read-only and share it across recording threads without locks. Lookups are
// O(1): flat arrays in the graph's arena, indexed by the virtual resource index.

#include "avis/graphics/buffer.h"
#include "avis/graphics/detail/render_pass_storage.h"
#include "avis/graphics/resource.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <span>

namespace avis::graphics
{
    namespace detail
    {
        // One virtual resource's physical side
        struct bag_entry
        {
            resource_kind kind = resource_kind::texture;
            bool imported = false;
            rhi::texture_handle physical_texture;
            rhi::buffer_handle physical_buffer;
            rhi::gpu_address address; // buffers
            // Transient resources: views in the frame context's descriptor range, created for the accesses that need
            // them (0, the null descriptor, otherwise). Imports use their persistent views.
            rhi::bindless_index srv;
            rhi::bindless_index buffer_uav;
            std::span<const rhi::bindless_index> texture_uavs; // per mip
            texture_description texture_info; // resolved: absolute size
            buffer_description buffer_info;
        };

        struct resource_bag_access;
    } // namespace detail

    class resource_bag
    {
    public:
        // Physical resources
        rhi::texture_handle texture_handle(texture resource) const;
        rhi::buffer_handle buffer_handle(buffer resource) const;
        rhi::gpu_address gpu_address(buffer resource) const;

        // Bindless indices: the frame context's descriptor range for transient resources (17 §4), the persistent ones
        // for imports
        rhi::bindless_index srv(texture resource) const;
        rhi::bindless_index uav(texture resource, std::uint32_t mip = 0) const;
        rhi::bindless_index srv(buffer resource) const;
        rhi::bindless_index uav(buffer resource) const;

        // Render target and depth views for set_render_targets
        rhi::texture_view render_target_view(texture resource, std::uint32_t mip = 0, std::uint32_t layer = 0) const;
        rhi::texture_view depth_stencil_view(texture resource) const;

        // The resolved description: relative sizes applied
        std::uint32_t width(texture resource) const;
        std::uint32_t height(texture resource) const;
        const texture_description& description(texture resource) const;
        const buffer_description& description(buffer resource) const;

    private:
        friend struct detail::resource_bag_access;

        resource_bag(rhi::resources resources, std::span<const detail::bag_entry> entries) noexcept :
            resources_{ resources },
            entries_{ entries }
        {}

        // The entry of a handle. Debug checks: a handle of this frame's graph, of the right kind, and declared by the
        // pass recording on this thread; a failed check returns an empty entry.
        const detail::bag_entry& find(const resource& handle, detail::resource_kind kind) const;

        rhi::resources resources_;
        std::span<const detail::bag_entry> entries_;
    };
} // namespace avis::graphics

#endif
