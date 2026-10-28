#ifndef AVIS_RHI_MEMORY_H
#define AVIS_RHI_MEMORY_H

// Heaps for placed resources (docs/design/09-rhi.md §8.5). The memory view follows with the resource manager.

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
} // namespace avis::rhi

#endif
