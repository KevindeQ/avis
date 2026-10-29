#ifndef AVIS_RHI_D3D12_RESOURCE_MANAGER_H
#define AVIS_RHI_D3D12_RESOURCE_MANAGER_H

// The owner of resources behind public handles: the memory allocator and the four resource slot maps
// (docs/design/09-rhi.md §21.6). The resources and memory views forward to it.

#include "avis/error.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/leaf_mutex.h"
#include "avis/rhi/d3d12/memory_allocator.h"
#include "avis/rhi/d3d12/resource_records.h"
#include "avis/rhi/d3d12/slot_map.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <memory>

namespace avis::rhi::d3d12
{
    class descriptor_heaps;
    class device;
    class lifetime_manager;

    class resource_manager
    {
    public:
        static result<std::unique_ptr<resource_manager>>
            create(device& device, descriptor_heaps& descriptors, lifetime_manager& lifetime);

        resource_manager(const resource_manager&) = delete;
        resource_manager& operator=(const resource_manager&) = delete;

        // Native object, then persistent descriptors (§8.3), then the slot map insert
        result<texture_handle> create_texture(const texture_description& description);
        result<buffer_handle> create_buffer(const buffer_description& description);
        result<sampler_handle> create_sampler(const sampler_description& description);
        result<heap_handle> create_heap(const heap_description& description);
        // No persistent descriptors (§8.4). A stale heap handle is invalid_handle.
        result<texture_handle>
            create_placed_texture(heap_handle heap, std::uint64_t offset, const texture_description& description);
        result<buffer_handle>
            create_placed_buffer(heap_handle heap, std::uint64_t offset, const buffer_description& description);
        // Committed with HEAP_FLAG_SHARED (§13); requires texture_usage::shared
        result<texture_handle> create_shared_texture(const texture_description& description);
        // Created on the first call, the same handle afterwards; the record owns it (§21.5)
        result<HANDLE> export_shared_handle(texture_handle texture);

        // Extract the record and hand it to lifetime_manager::destroy (§23.8.1). A stale or invalid handle is a no-op
        // and asserts in debug builds. Placed resources decrement their heap's live_resources first; destroy(heap)
        // asserts that count is 0.
        void destroy(texture_handle texture) noexcept;
        void destroy(buffer_handle buffer) noexcept;
        void destroy(sampler_handle sampler) noexcept;
        void destroy(heap_handle heap) noexcept;

        // Lock-free lookups for recording and views (§21.2); nullptr for a stale handle
        const texture_record* find(texture_handle texture) const noexcept;
        const buffer_record* find(buffer_handle buffer) const noexcept;
        const sampler_record* find(sampler_handle sampler) const noexcept;
        const heap_record* find(heap_handle heap) const noexcept;

        // Slots ever used by the texture and buffer maps (slot_map::slots_used), for stress tests
        std::uint32_t texture_slots_used() const noexcept;
        std::uint32_t buffer_slots_used() const noexcept;

        // Records not behind a public handle: transient objects' buffers (§23.6), timestamp resolve buffers (§24.4).
        // No persistent descriptors.
        result<buffer_record> create_internal_buffer(const buffer_description& description);

        allocation_requirements requirements(const texture_description& description) const;
        allocation_requirements requirements(const buffer_description& description) const;
        memory_budget local_budget() const;

    private:
        resource_manager(device& device, descriptor_heaps& descriptors, lifetime_manager& lifetime) noexcept;

        // `shared`: the description is for create_shared_texture, which alone accepts texture_usage::shared
        result<void> validate(const texture_description& description, bool shared = false) const;
        result<void> validate(const buffer_description& description) const;

        // The persistent views of §8.3
        result<void> create_views(texture_record& record);
        result<void> create_views(buffer_record& record);

        device& device_;
        descriptor_heaps& descriptors_;
        lifetime_manager& lifetime_;
        std::unique_ptr<memory_allocator> allocator_;
        slot_map<texture_record> textures_;
        slot_map<buffer_record> buffers_;
        slot_map<sampler_record> samplers_;
        slot_map<heap_record> heaps_;
        leaf_mutex export_mutex_{ "resource_manager::export_mutex_" }; // export_shared_handle's first call
    };
} // namespace avis::rhi::d3d12

#endif
