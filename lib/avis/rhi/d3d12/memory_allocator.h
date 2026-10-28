#ifndef AVIS_RHI_D3D12_MEMORY_ALLOCATOR_H
#define AVIS_RHI_D3D12_MEMORY_ALLOCATOR_H

// Wraps D3D12MA::Allocator (docs/design/09-rhi.md §8.2, §21.4). Every resource is created with an initial barrier
// layout (CreateResource3): Avis uses enhanced barriers throughout. The records come back without descriptors; the
// resource manager adds them.

#include "avis/error.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/resource_records.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/resources.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace avis::rhi::d3d12
{
    class device;

    class memory_allocator
    {
    public:
        static result<std::unique_ptr<memory_allocator>> create(device& device);

        memory_allocator(const memory_allocator&) = delete;
        memory_allocator& operator=(const memory_allocator&) = delete;

        result<texture_record> create_texture(const texture_description& description);
        // Upload and readback buffers are mapped once here and stay mapped until released
        result<buffer_record> create_buffer(const buffer_description& description);

        // A dedicated heap of `description.size` rounded up to 64 KiB
        result<std::shared_ptr<placed_heap>> create_heap(const heap_description& description);

        // CreateAliasingResource2 at `offset`, initial layout undefined (§8.5). The record shares ownership of `heap`
        // and counts itself in live_resources.
        result<texture_record> create_placed_texture(
            std::shared_ptr<placed_heap> heap, std::uint64_t offset, const texture_description& description);
        result<buffer_record> create_placed_buffer(
            std::shared_ptr<placed_heap> heap, std::uint64_t offset, const buffer_description& description);

        // Committed with HEAP_FLAG_SHARED (§13); the resource manager's create_shared_texture comes with 6.11
        result<texture_record> create_shared_texture(const texture_description& description);

        // GetResourceAllocationInfo2
        allocation_requirements requirements(const D3D12_RESOURCE_DESC1& description) const;

        // The budget's only owner: D3D12MA::Allocator::GetBudget for the local segment. Warns on the "rhi" logger,
        // at most once a second, when usage passes 90% of the budget.
        memory_budget local_budget() const;

    private:
        explicit memory_allocator(device& device) noexcept;

        result<texture_record>
            create_texture(const texture_description& description, D3D12_HEAP_FLAGS extra_heap_flags, bool committed);

        device& device_;
        com_ptr<D3D12MA::Allocator> allocator_;
        mutable std::atomic<std::int64_t> last_budget_warning_{ 0 }; // steady_clock ticks
    };
} // namespace avis::rhi::d3d12

#endif
