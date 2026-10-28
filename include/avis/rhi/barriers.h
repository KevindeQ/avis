#ifndef AVIS_RHI_BARRIERS_H
#define AVIS_RHI_BARRIERS_H

// Enhanced barriers (docs/design/09-rhi.md §10). The types map 1:1 to D3D12_TEXTURE_BARRIER, D3D12_BUFFER_BARRIER
// and D3D12_GLOBAL_BARRIER. The RHI doesn't track state: callers name both sides of every barrier (§10.1).

#include "avis/enum_flags.h"
#include "avis/rhi/types.h"

#include <cstdint>

namespace avis::rhi
{
    // Values equal D3D12_BARRIER_SYNC's, so the conversion is a cast (§20.2.3)
    enum class pipeline_synchronization : std::uint32_t
    {
        none = 0,
        all = 0x1,
        draw = 0x2,
        index_input = 0x4,
        vertex_shading = 0x8,
        pixel_shading = 0x10,
        depth_stencil = 0x20,
        render_target = 0x40,
        compute_shading = 0x80,
        copy = 0x200,
        execute_indirect = 0x800,
        clear_unordered_access_view = 0x8000
    };

    // Values equal D3D12_BARRIER_ACCESS's. `common` is 0, so an empty set means COMMON, not "no access".
    enum class resource_access : std::uint32_t
    {
        common = 0,
        vertex_buffer = 0x1,
        constant_buffer = 0x2,
        index_buffer = 0x4,
        render_target = 0x8,
        unordered_access = 0x10,
        depth_write = 0x20,
        depth_read = 0x40,
        shader_resource = 0x80,
        indirect_argument = 0x200,
        copy_destination = 0x400,
        copy_source = 0x800,
        no_access = 0x8000'0000
    };

    // D3D12_BARRIER_LAYOUT, through a table (§20.2.3)
    enum class texture_layout : std::uint8_t
    {
        undefined,
        common,
        shader_resource,
        unordered_access,
        render_target,
        depth_read,
        depth_write,
        copy_source,
        copy_destination,
        direct_queue_common,
        direct_queue_shader_resource,
        direct_queue_unordered_access,
        compute_queue_common,
        compute_queue_shader_resource,
        compute_queue_unordered_access
    };
} // namespace avis::rhi

ALLOW_FLAGS_FOR_ENUM(avis::rhi::pipeline_synchronization)
ALLOW_FLAGS_FOR_ENUM(avis::rhi::resource_access)

namespace avis::rhi
{
    // The default {none, no_access, common} is valid in D3D12: SYNC_NONE requires ACCESS_NO_ACCESS
    struct access_state
    {
        avis::flags<pipeline_synchronization> synchronization = pipeline_synchronization::none;
        avis::flags<resource_access> access = resource_access::no_access;
        texture_layout layout = texture_layout::common; // ignored for buffers and global barriers
    };

    // Common states, for imports into the render graph (10 §3.6) and code outside it (uploads, interop). The enums
    // are passed positionally: brace-initialising flags from one enum picks the initializer-list constructor, which
    // isn't constexpr.
    namespace access_states
    {
        inline constexpr access_state undefined{ pipeline_synchronization::none,
                                                 resource_access::no_access,
                                                 texture_layout::undefined };
        inline constexpr access_state common{ pipeline_synchronization::all,
                                              resource_access::common,
                                              texture_layout::common };
        inline constexpr access_state shader_resource{ pipeline_synchronization::pixel_shading |
                                                           pipeline_synchronization::compute_shading,
                                                       resource_access::shader_resource,
                                                       texture_layout::shader_resource };
        inline constexpr access_state unordered_access{ pipeline_synchronization::compute_shading,
                                                        resource_access::unordered_access,
                                                        texture_layout::unordered_access };
        inline constexpr access_state render_target{ pipeline_synchronization::render_target,
                                                     resource_access::render_target,
                                                     texture_layout::render_target };
        inline constexpr access_state depth_write{ pipeline_synchronization::depth_stencil,
                                                   resource_access::depth_write,
                                                   texture_layout::depth_write };
        inline constexpr access_state copy_source{ pipeline_synchronization::copy,
                                                   resource_access::copy_source,
                                                   texture_layout::copy_source };
        inline constexpr access_state copy_destination{ pipeline_synchronization::copy,
                                                        resource_access::copy_destination,
                                                        texture_layout::copy_destination };
    } // namespace access_states

    struct texture_barrier
    {
        texture_handle texture;
        subresource_range range = all_subresources;
        access_state before;
        access_state after;
        bool discard = false; // first use of an aliased render or depth target (§10.1)
    };

    struct buffer_barrier
    {
        buffer_handle buffer;
        access_state before;
        access_state after;
    };

    struct global_barrier
    {
        access_state before;
        access_state after;
    };
} // namespace avis::rhi

#endif
