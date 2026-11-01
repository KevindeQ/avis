#ifndef AVIS_VISUALIZER_RENDERING_GEOMETRY_ARENA_H
#define AVIS_VISUALIZER_RENDERING_GEOMETRY_ARENA_H

// The renderer's geometry arena (docs/design/11-renderer.md §3.2, §3.6, docs/tasks/11-renderer.md §11.3): one vertex
// buffer and one index buffer, both gpu_only and persistent, each sub-allocated by an offset allocator. Binding a
// different mesh only changes root constants.
//
// geometry.arena_mib is split 80% vertices, 20% indices (the task's Open point "Arena split").
//
// Vertex streams are laid out per range as structure of arrays: positions, then each present attribute in the order
// normal, tangent, uv0, color0, each stream starting on a 16-byte boundary. Per vertex:
//   position  f32x3 (12 bytes)
//   normal    octahedral u16x2 (4 bytes)
//   tangent   octahedral u16x2 (4 bytes); bit 0 of the second component is the bitangent sign, 1 = −1
//   uv0       f16x2 (4 bytes)
//   color0    sRGB u8x4 (4 bytes)
// Indices are u32, relative to the primitive's first vertex.
//
// Not thread-safe: the frame chain's prepare stage is the only user.

#include "avis/enum_flags.h"
#include "avis/error.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"
#include "avis/scene/assets/mesh_data.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>

namespace avis::visualizer::rendering
{
    // Best-fit sub-allocation of [0, capacity) (11.3). Sizes and offsets are rounded to `alignment`, a power of two.
    // The free list is ordered by offset, so freeing coalesces with both neighbours; a second index by size gives the
    // best fit, ties going to the lowest offset.
    class offset_allocator
    {
    public:
        // The capacity is rounded down to the alignment
        offset_allocator(std::uint64_t capacity, std::uint64_t alignment);

        // The offset of `size` bytes (rounded up to the alignment), or nullopt when no free block fits. A size of 0 is
        // a debug check and gives nullopt.
        std::optional<std::uint64_t> allocate(std::uint64_t size);

        // Frees the allocation at `offset`; an offset that isn't allocated is a debug check
        void free(std::uint64_t offset) noexcept;

        std::uint64_t capacity() const noexcept
        {
            return capacity_;
        }

        std::uint64_t alignment() const noexcept
        {
            return alignment_;
        }

        std::uint64_t used_bytes() const noexcept
        {
            return used_;
        }

        std::uint64_t free_bytes() const noexcept
        {
            return capacity_ - used_;
        }

        std::size_t allocation_count() const noexcept
        {
            return allocations_.size();
        }

        std::size_t free_block_count() const noexcept
        {
            return free_by_offset_.size();
        }

        std::uint64_t largest_free_block() const noexcept;

        // The rounded size of the allocation at `offset`, or 0 when nothing is allocated there
        std::uint64_t allocation_size(std::uint64_t offset) const noexcept;

    private:
        void insert_free(std::uint64_t offset, std::uint64_t size);
        void erase_free(std::map<std::uint64_t, std::uint64_t>::iterator block);

        std::uint64_t capacity_ = 0;
        std::uint64_t alignment_ = 1;
        std::uint64_t used_ = 0;
        std::map<std::uint64_t, std::uint64_t> free_by_offset_; // offset → size
        std::set<std::pair<std::uint64_t, std::uint64_t>> free_by_size_; // (size, offset)
        std::map<std::uint64_t, std::uint64_t> allocations_; // offset → rounded size
    };

    // Byte offsets of the vertex streams, relative to the start of a range; absent_stream for a stream the mesh
    // doesn't have
    struct vertex_stream_offsets
    {
        static constexpr std::uint32_t absent_stream = ~0u;

        std::uint32_t position = 0;
        std::uint32_t normal = absent_stream;
        std::uint32_t tangent = absent_stream;
        std::uint32_t uv0 = absent_stream;
        std::uint32_t color0 = absent_stream;

        friend bool operator==(const vertex_stream_offsets&, const vertex_stream_offsets&) = default;
    };

    inline constexpr std::uint64_t vertex_stream_alignment = 16;
    inline constexpr std::uint64_t index_alignment = 4;
    inline constexpr std::uint32_t position_stride = 12;
    inline constexpr std::uint32_t attribute_stride = 4; // normal, tangent, uv0 and color0
    inline constexpr std::uint32_t index_stride = 4;

    // The streams of `vertex_count` vertices with `attributes`, and their total size (each stream padded to 16 bytes)
    struct vertex_stream_layout
    {
        vertex_stream_offsets offsets;
        std::uint64_t size = 0;
    };

    vertex_stream_layout
        layout_vertex_streams(std::uint32_t vertex_count, avis::flags<scene::vertex_attribute> attributes) noexcept;

    // One mesh version's place in the arena. Offsets are in bytes from the start of each buffer. A range without
    // indices has index_size 0.
    struct geometry_range
    {
        std::uint32_t vertex_offset = 0;
        std::uint32_t vertex_size = 0; // as allocated (a multiple of 16)
        std::uint32_t index_offset = 0;
        std::uint32_t index_size = 0;
        vertex_stream_offsets streams; // absolute: vertex_offset already added

        // The first index of the range, for draw_indexed's start index
        std::uint32_t first_index() const noexcept
        {
            return index_offset / index_stride;
        }

        friend bool operator==(const geometry_range&, const geometry_range&) = default;
    };

    struct geometry_arena_capacity
    {
        std::uint64_t vertex_bytes = 0;
        std::uint64_t index_bytes = 0;
    };

    // geometry.arena_mib split 80% vertices, 20% indices, aligned to their alignments
    geometry_arena_capacity split_geometry_arena(std::uint32_t arena_mib) noexcept;

    struct geometry_arena_statistics
    {
        std::uint64_t vertex_capacity = 0;
        std::uint64_t vertex_used = 0;
        std::uint64_t index_capacity = 0;
        std::uint64_t index_used = 0;
        std::size_t ranges = 0;
        std::uint64_t largest_free_vertex_block = 0;
        std::uint64_t largest_free_index_block = 0;
    };

    // Which of the two buffers stopped an allocation
    enum class arena_exhausted : std::uint8_t
    {
        none,
        vertices,
        indices
    };

    class geometry_arena
    {
    public:
        // Creates both buffers. invalid_argument when a buffer would be empty or reach 4 GiB (offsets are 32-bit, the
        // shaders' ByteAddressBuffer offsets); the RHI's errors pass through.
        static result<std::unique_ptr<geometry_arena>>
            create(rhi::resources resources, const geometry_arena_capacity& capacity);

        // Destroys the buffers; the RHI's deferred release keeps them while frames use them
        ~geometry_arena();

        geometry_arena(const geometry_arena&) = delete;
        geometry_arena& operator=(const geometry_arena&) = delete;

        // A range of `vertex_size` vertex bytes and `index_size` index bytes, or nullopt when either buffer has no
        // block that fits (nothing is allocated then; last_exhausted() says which). vertex_size must be non-zero.
        std::optional<geometry_range> allocate(std::uint64_t vertex_size, std::uint64_t index_size);

        // A range for the streams of `layout`, with its stream offsets filled in
        std::optional<geometry_range> allocate(const vertex_stream_layout& layout, std::uint64_t index_size);

        void free(const geometry_range& range) noexcept;

        // Of the last allocate that returned nullopt
        arena_exhausted last_exhausted() const noexcept
        {
            return last_exhausted_;
        }

        // Raw (ByteAddressBuffer) shader resources that copies can write; the index buffer is also an index buffer
        rhi::buffer_handle vertex_buffer() const noexcept
        {
            return vertex_buffer_;
        }

        rhi::buffer_handle index_buffer() const noexcept
        {
            return index_buffer_;
        }

        geometry_arena_statistics statistics() const noexcept;

    private:
        geometry_arena(
            rhi::resources resources,
            const geometry_arena_capacity& capacity,
            rhi::buffer_handle vertex_buffer,
            rhi::buffer_handle index_buffer);

        rhi::resources resources_;
        rhi::buffer_handle vertex_buffer_;
        rhi::buffer_handle index_buffer_;
        offset_allocator vertices_;
        offset_allocator indices_;
        arena_exhausted last_exhausted_ = arena_exhausted::none;
    };
} // namespace avis::visualizer::rendering

#endif
