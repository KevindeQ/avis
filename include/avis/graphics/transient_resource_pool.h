#ifndef AVIS_GRAPHICS_TRANSIENT_RESOURCE_POOL_H
#define AVIS_GRAPHICS_TRANSIENT_RESOURCE_POOL_H

// Heaps and placed resources for the render graph's transient resources (docs/design/10-render-graph.md §6.4 steps
// 3–4, D-95). One heap per frame context, grown in 64 MiB steps and shrunk after a while below its high-water mark, and
// a cache of placed resources keyed by (description, heap, offset), so a steady-state frame creates nothing.
// Not thread-safe: the graph calls it from one thread at a time (compile, then materialise).

#include "avis/error.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace avis::graphics
{
    // Counters for tests and the debug overlay
    struct transient_pool_statistics
    {
        std::uint64_t textures_created = 0;
        std::uint64_t buffers_created = 0;
        std::uint64_t resources_destroyed = 0; // evicted, or with their heap
        std::uint64_t heaps_created = 0;
        std::uint64_t heaps_destroyed = 0;
    };

    class transient_resource_pool
    {
    public:
        static constexpr std::uint64_t heap_granularity = std::uint64_t{ 64 } << 20; // 64 MiB
        // Frames a placed resource may go unused before it is destroyed, and a heap may stay below its high-water mark
        // before it shrinks
        static constexpr std::uint32_t retention_frames = 120;

        // frames_in_flight: the frame scheduler's, one heap per frame context
        transient_resource_pool(rhi::memory memory, rhi::resources resources, std::uint32_t frames_in_flight);

        // Destroys every cached resource, then the heaps (deferred releases). No frame may be executing.
        ~transient_resource_pool();

        transient_resource_pool(const transient_resource_pool&) = delete;
        transient_resource_pool& operator=(const transient_resource_pool&) = delete;

        std::uint32_t frames_in_flight() const noexcept
        {
            return static_cast<std::uint32_t>(contexts_.size());
        }

        // rhi::memory::requirements, cached by description (the compiler asks per resource per frame). The debug name
        // isn't part of the key.
        rhi::allocation_requirements requirements(const rhi::texture_description& description);
        rhi::allocation_requirements requirements(const rhi::buffer_description& description);

        // Once per frame, before acquiring: makes the context's heap at least `heap_size` bytes. Too small, it is
        // replaced by one rounded up to 64 MiB (its cached resources and the old heap are destroyed through the
        // deferred release). After retention_frames frames below its size, it shrinks to the largest size those frames
        // needed, rounded up to 64 MiB. Also destroys resources unused for retention_frames frames. Fails with the
        // RHI's error when the heap can't be created; the context then has no heap.
        result<void> prepare(std::uint32_t context_index, std::uint64_t heap_size);

        // A placed resource of the context's heap at `offset` (aligned to requirements(description).alignment): the
        // cached one for this description and offset, or a new one
        result<rhi::texture_handle> acquire_texture(
            std::uint32_t context_index, const rhi::texture_description& description, std::uint64_t offset);
        result<rhi::buffer_handle> acquire_buffer(
            std::uint32_t context_index, const rhi::buffer_description& description, std::uint64_t offset);

        // The context's heap and its size; an invalid handle and 0 before the first prepare that needed memory
        rhi::heap_handle heap(std::uint32_t context_index) const noexcept;
        std::uint64_t heap_size(std::uint32_t context_index) const noexcept;
        // The largest heap_size a prepare of the context asked for
        std::uint64_t high_water_mark(std::uint32_t context_index) const noexcept;

        const transient_pool_statistics& statistics() const noexcept
        {
            return statistics_;
        }

    private:
        struct cache_key
        {
            std::size_t description_hash = 0;
            std::uint64_t offset = 0;
            bool texture = true;

            friend bool operator==(const cache_key&, const cache_key&) noexcept = default;
        };

        struct cache_key_hash
        {
            std::size_t operator()(const cache_key& key) const noexcept;
        };

        // Debug names aren't kept (they point into the graph's arena)
        struct cached_resource
        {
            rhi::texture_description texture;
            rhi::buffer_description buffer;
            rhi::texture_handle texture_handle;
            rhi::buffer_handle buffer_handle;
            std::uint64_t last_used = 0; // the frame counter when last acquired
        };

        struct frame_context
        {
            rhi::heap_handle heap;
            std::uint64_t size = 0;
            std::uint64_t high_water_mark = 0;
            std::uint32_t frames_below = 0; // consecutive frames that needed less than size
            std::uint64_t recent_peak = 0; // the most those frames needed
            std::unordered_map<cache_key, cached_resource, cache_key_hash> cache;
        };

        struct texture_requirements
        {
            rhi::texture_description description;
            rhi::allocation_requirements requirements;
        };

        struct buffer_requirements
        {
            rhi::buffer_description description;
            rhi::allocation_requirements requirements;
        };

        frame_context* context(std::uint32_t context_index) noexcept;
        const frame_context* context(std::uint32_t context_index) const noexcept;
        void destroy_resource(cached_resource& resource) noexcept;
        void destroy_cache(frame_context& context) noexcept;
        // Replaces the context's heap with one of `size` bytes (none for 0)
        result<void> replace_heap(std::uint32_t context_index, frame_context& context, std::uint64_t size);

        rhi::memory memory_;
        rhi::resources resources_;
        std::vector<frame_context> contexts_;
        std::unordered_map<std::size_t, texture_requirements> texture_requirements_;
        std::unordered_map<std::size_t, buffer_requirements> buffer_requirements_;
        std::uint64_t frame_ = 0; // prepare() calls so far
        transient_pool_statistics statistics_;
    };
} // namespace avis::graphics

#endif
