#ifndef AVIS_GRAPHICS_TRANSIENT_RESOURCE_POOL_H
#define AVIS_GRAPHICS_TRANSIENT_RESOURCE_POOL_H

// Heaps and placed resources for the render graph's transient resources (docs/design/10-render-graph.md §6.4, D-95).
// So far only the views and the frame count it is created from; heaps, the placed-resource cache and the requirements
// cache come with task 9.7.

#include "avis/rhi/memory.h"
#include "avis/rhi/resources.h"

#include <cstdint>

namespace avis::graphics
{
    class transient_resource_pool
    {
    public:
        // frames_in_flight: the frame scheduler's, one heap per frame context
        transient_resource_pool(
            const rhi::memory memory, const rhi::resources resources, const std::uint32_t frames_in_flight) noexcept :
            memory_{ memory },
            resources_{ resources },
            frames_in_flight_{ frames_in_flight }
        {}

        transient_resource_pool(const transient_resource_pool&) = delete;
        transient_resource_pool& operator=(const transient_resource_pool&) = delete;

        std::uint32_t frames_in_flight() const noexcept
        {
            return frames_in_flight_;
        }

    private:
        rhi::memory memory_;
        rhi::resources resources_;
        std::uint32_t frames_in_flight_;
    };
} // namespace avis::graphics

#endif
