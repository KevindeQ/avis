#ifndef AVIS_GRAPHICS_GRAPH_DEBUG_INFORMATION_H
#define AVIS_GRAPHICS_GRAPH_DEBUG_INFORMATION_H

// What the debug UI shows about a compiled render graph (docs/design/10-render-graph.md §9): its passes and resources
// with qualified names. Everything points into the graph's arena and is valid until the next reset(). Free of D3D12
// and <windows.h>.

#include "avis/graphics/buffer.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace avis::graphics
{
    struct pass_debug_information
    {
        std::string_view name; // qualified with its view
        pass_type type = pass_type::graphics;
        rhi::queue_type queue = rhi::queue_type::graphics; // graphics for culled passes
        bool culled = false;
        // The pass's GPU time in frame N − F, from the profiler's completed timings; set by execute when the graph has
        // a profiler and the timings exist
        std::optional<double> gpu_milliseconds;
    };

    struct resource_debug_information
    {
        static constexpr std::uint64_t no_offset = std::numeric_limits<std::uint64_t>::max();
        static constexpr std::uint32_t no_position = std::numeric_limits<std::uint32_t>::max();

        std::string_view name; // qualified with its view
        bool is_texture = true;
        texture_description texture; // textures: the size resolved to absolute
        buffer_description buffer; // buffers
        bool imported = false;
        bool debug_selected = false; // read for the debug view: kept out of aliasing
        std::span<const std::uint32_t> producers; // passes writing it, in declaration order (culled ones included)
        std::span<const std::uint32_t> consumers; // passes reading it, in declaration order (culled ones included)
        std::uint64_t heap_offset = no_offset; // live transient resources
        bool aliased = false; // shares heap memory with another resource
        // Positions in the order of passes that run, widened for async compute; no_position when nothing uses it
        std::uint32_t first_use = no_position;
        std::uint32_t last_use = no_position;
    };

    struct graph_debug_information
    {
        std::span<const pass_debug_information> passes; // in declaration order
        std::span<const resource_debug_information> resources; // in creation order
        std::uint64_t transient_heap_size = 0;
        std::uint64_t aliased_bytes = 0;
        std::uint64_t non_aliased_bytes = 0;
        std::uint32_t recording_groups = 0; // of the last execute
    };
} // namespace avis::graphics

#endif
