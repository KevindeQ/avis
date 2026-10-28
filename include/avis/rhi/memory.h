#ifndef AVIS_RHI_MEMORY_H
#define AVIS_RHI_MEMORY_H

// Heaps for placed resources and the memory view (docs/design/09-rhi.md §4.3, §8.5, D-95). Free of D3D12 and
// <windows.h>.

#include "avis/error.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <string_view>

namespace avis::rhi
{
    enum class heap_kind : std::uint8_t
    {
        all_resources, // buffers, textures and render targets (resource heap tier 2, required)
        buffers,
        render_targets_and_textures
    };

    struct heap_description
    {
        std::uint64_t size = 0; // rounded up to 64 KiB
        heap_kind kind = heap_kind::all_resources;
        std::string_view debug_name; // copied at creation
    };

    // GetResourceAllocationInfo2 for a description, so the render graph can plan placements before creating
    struct allocation_requirements
    {
        std::uint64_t size = 0;
        std::uint64_t alignment = 0; // 64 KiB for everything Avis uses (no MSAA)
    };

    class render_hardware_interface;

    namespace d3d12
    {
        class device_state;
    } // namespace d3d12

    // A view (§4.3) for placed resources, for aliasing (D-95). Only the render graph's transient pool needs it. Placed
    // textures and buffers are ordinary handles afterwards: their views come from a descriptor_range (§6), and they
    // are destroyed through resources::destroy.
    class memory
    {
    public:
        explicit memory(render_hardware_interface& hardware_interface) noexcept;

        allocation_requirements requirements(const texture_description& description) const;
        allocation_requirements requirements(const buffer_description& description) const;

        result<heap_handle> create_heap(const heap_description& description);
        // Deferred. No resource placed in it may still be alive (asserted); destroying them first, even just before,
        // is enough (§8.5).
        void destroy(heap_handle heap) noexcept;

        // The offset must respect requirements(description).alignment. Initial layout undefined: the first use is an
        // aliasing barrier with discard (§10.1).
        result<texture_handle>
            create_placed_texture(heap_handle heap, std::uint64_t offset, const texture_description& description);
        result<buffer_handle>
            create_placed_buffer(heap_handle heap, std::uint64_t offset, const buffer_description& description);

    private:
        d3d12::device_state* state_;
    };
} // namespace avis::rhi

#endif
