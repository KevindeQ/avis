#include "rendering/geometry_arena.h"

#include "avis/detail/check.h"
#include "avis/graphics/error.h"

#include <format>
#include <limits>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "renderer";

        constexpr std::uint64_t align_up(const std::uint64_t value, const std::uint64_t alignment) noexcept
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        constexpr std::uint64_t align_down(const std::uint64_t value, const std::uint64_t alignment) noexcept
        {
            return value & ~(alignment - 1);
        }

        // Offsets are 32-bit (geometry_range, and the shaders' ByteAddressBuffer offsets)
        constexpr std::uint64_t maximum_buffer_size = std::uint64_t{ std::numeric_limits<std::uint32_t>::max() } + 1;
    } // namespace

    // ---- offset_allocator ----

    offset_allocator::offset_allocator(const std::uint64_t capacity, const std::uint64_t alignment) :
        capacity_{ align_down(capacity, alignment) },
        alignment_{ alignment }
    {
        AVIS_CHECK(
            logger_name,
            alignment != 0 && (alignment & (alignment - 1)) == 0,
            "offset_allocator: the alignment must be a power of two");
        if (capacity_ > 0)
        {
            insert_free(0, capacity_);
        }
    }

    std::optional<std::uint64_t> offset_allocator::allocate(const std::uint64_t size)
    {
        if (!AVIS_CHECK(logger_name, size > 0, "offset_allocator: an allocation of 0 bytes"))
        {
            return std::nullopt;
        }
        const std::uint64_t rounded = align_up(size, alignment_);
        if (rounded < size)
        {
            return std::nullopt; // overflow
        }

        // Best fit: the smallest free block that holds it, the lowest offset among equal sizes
        const auto fit = free_by_size_.lower_bound({ rounded, 0 });
        if (fit == free_by_size_.end())
        {
            return std::nullopt;
        }
        const std::uint64_t block_size = fit->first;
        const std::uint64_t offset = fit->second;
        erase_free(free_by_offset_.find(offset));
        if (block_size > rounded)
        {
            insert_free(offset + rounded, block_size - rounded);
        }

        allocations_.emplace(offset, rounded);
        used_ += rounded;
        return offset;
    }

    void offset_allocator::free(const std::uint64_t offset) noexcept
    {
        const auto allocation = allocations_.find(offset);
        if (!AVIS_CHECK(
                logger_name, allocation != allocations_.end(), "offset_allocator: freeing an offset not allocated"))
        {
            return;
        }
        std::uint64_t begin = offset;
        std::uint64_t size = allocation->second;
        allocations_.erase(allocation);
        used_ -= size;

        // Coalesce with the free neighbours on both sides
        auto next = free_by_offset_.lower_bound(begin);
        if (next != free_by_offset_.end() && next->first == begin + size)
        {
            size += next->second;
            const auto after = std::next(next);
            erase_free(next);
            next = after;
        }
        if (next != free_by_offset_.begin())
        {
            const auto previous = std::prev(next);
            if (previous->first + previous->second == begin)
            {
                begin = previous->first;
                size += previous->second;
                erase_free(previous);
            }
        }
        insert_free(begin, size);
    }

    std::uint64_t offset_allocator::largest_free_block() const noexcept
    {
        return free_by_size_.empty() ? 0 : free_by_size_.rbegin()->first;
    }

    std::uint64_t offset_allocator::allocation_size(const std::uint64_t offset) const noexcept
    {
        const auto found = allocations_.find(offset);
        return found == allocations_.end() ? 0 : found->second;
    }

    void offset_allocator::insert_free(const std::uint64_t offset, const std::uint64_t size)
    {
        free_by_offset_.emplace(offset, size);
        free_by_size_.emplace(size, offset);
    }

    void offset_allocator::erase_free(const std::map<std::uint64_t, std::uint64_t>::iterator block)
    {
        free_by_size_.erase({ block->second, block->first });
        free_by_offset_.erase(block);
    }

    // ---- stream layout ----

    vertex_stream_layout layout_vertex_streams(
        const std::uint32_t vertex_count, const avis::flags<scene::vertex_attribute> attributes) noexcept
    {
        vertex_stream_layout layout;
        std::uint64_t end = 0;
        const auto place = [&](const std::uint32_t stride) -> std::uint32_t
        {
            const std::uint64_t offset = end;
            end = align_up(offset + std::uint64_t{ stride } * vertex_count, vertex_stream_alignment);
            return static_cast<std::uint32_t>(offset);
        };

        layout.offsets.position = place(position_stride);
        if (attributes & scene::vertex_attribute::normal)
        {
            layout.offsets.normal = place(attribute_stride);
        }
        if (attributes & scene::vertex_attribute::tangent)
        {
            layout.offsets.tangent = place(attribute_stride);
        }
        if (attributes & scene::vertex_attribute::uv0)
        {
            layout.offsets.uv0 = place(attribute_stride);
        }
        if (attributes & scene::vertex_attribute::color0)
        {
            layout.offsets.color0 = place(attribute_stride);
        }
        layout.size = end;
        return layout;
    }

    geometry_arena_capacity split_geometry_arena(const std::uint32_t arena_mib) noexcept
    {
        const std::uint64_t total = std::uint64_t{ arena_mib } << 20;
        const std::uint64_t vertex_bytes = align_down(total / 5 * 4, vertex_stream_alignment);
        return { .vertex_bytes = vertex_bytes, .index_bytes = align_down(total - vertex_bytes, index_alignment) };
    }

    // ---- geometry_arena ----

    result<std::unique_ptr<geometry_arena>>
        geometry_arena::create(rhi::resources resources, const geometry_arena_capacity& capacity)
    {
        for (const auto& [name, size] :
             { std::pair{ "vertex", capacity.vertex_bytes }, std::pair{ "index", capacity.index_bytes } })
        {
            if (size == 0 || size >= maximum_buffer_size)
            {
                return std::unexpected{ graphics::make_error(
                    graphics::error_kind::invalid_argument,
                    std::format(
                        "geometry arena: the {} buffer needs 1 byte to 4 GiB - 1, got {} bytes "
                        "(render.geometry.arena_mib)",
                        name,
                        size)) };
            }
        }

        auto vertices = resources.create_buffer(
            { .size = capacity.vertex_bytes,
              .usage = rhi::buffer_usage::shader_resource | rhi::buffer_usage::copy_destination,
              .memory = rhi::memory_type::gpu_only,
              .debug_name = "geometry arena vertices" });
        if (!vertices)
        {
            return std::unexpected{ std::move(vertices).error() };
        }
        auto indices = resources.create_buffer(
            { .size = capacity.index_bytes,
              .usage =
                  rhi::buffer_usage::index | rhi::buffer_usage::shader_resource | rhi::buffer_usage::copy_destination,
              .memory = rhi::memory_type::gpu_only,
              .debug_name = "geometry arena indices" });
        if (!indices)
        {
            resources.destroy(*vertices);
            return std::unexpected{ std::move(indices).error() };
        }
        return std::unique_ptr<geometry_arena>{ new geometry_arena{ resources, capacity, *vertices, *indices } };
    }

    geometry_arena::geometry_arena(
        rhi::resources resources,
        const geometry_arena_capacity& capacity,
        const rhi::buffer_handle vertex_buffer,
        const rhi::buffer_handle index_buffer) :
        resources_{ resources },
        vertex_buffer_{ vertex_buffer },
        index_buffer_{ index_buffer },
        vertices_{ capacity.vertex_bytes, vertex_stream_alignment },
        indices_{ capacity.index_bytes, index_alignment }
    {}

    geometry_arena::~geometry_arena()
    {
        resources_.destroy(vertex_buffer_);
        resources_.destroy(index_buffer_);
    }

    std::optional<geometry_range>
        geometry_arena::allocate(const std::uint64_t vertex_size, const std::uint64_t index_size)
    {
        const std::optional<std::uint64_t> vertex_offset = vertices_.allocate(vertex_size);
        if (!vertex_offset)
        {
            last_exhausted_ = arena_exhausted::vertices;
            return std::nullopt;
        }
        std::optional<std::uint64_t> index_offset = 0;
        if (index_size > 0)
        {
            index_offset = indices_.allocate(index_size);
            if (!index_offset)
            {
                vertices_.free(*vertex_offset);
                last_exhausted_ = arena_exhausted::indices;
                return std::nullopt;
            }
        }

        geometry_range range;
        range.vertex_offset = static_cast<std::uint32_t>(*vertex_offset);
        range.vertex_size = static_cast<std::uint32_t>(vertices_.allocation_size(*vertex_offset));
        range.index_offset = static_cast<std::uint32_t>(*index_offset);
        range.index_size = index_size > 0 ? static_cast<std::uint32_t>(indices_.allocation_size(*index_offset)) : 0;
        range.streams.position = range.vertex_offset;
        return range;
    }

    std::optional<geometry_range>
        geometry_arena::allocate(const vertex_stream_layout& layout, const std::uint64_t index_size)
    {
        std::optional<geometry_range> range = allocate(layout.size, index_size);
        if (range)
        {
            const auto absolute = [&](const std::uint32_t offset)
            { return offset == vertex_stream_offsets::absent_stream ? offset : range->vertex_offset + offset; };
            range->streams = { .position = absolute(layout.offsets.position),
                               .normal = absolute(layout.offsets.normal),
                               .tangent = absolute(layout.offsets.tangent),
                               .uv0 = absolute(layout.offsets.uv0),
                               .color0 = absolute(layout.offsets.color0) };
        }
        return range;
    }

    void geometry_arena::free(const geometry_range& range) noexcept
    {
        vertices_.free(range.vertex_offset);
        if (range.index_size > 0)
        {
            indices_.free(range.index_offset);
        }
    }

    geometry_arena_statistics geometry_arena::statistics() const noexcept
    {
        return {
            .vertex_capacity = vertices_.capacity(),
            .vertex_used = vertices_.used_bytes(),
            .index_capacity = indices_.capacity(),
            .index_used = indices_.used_bytes(),
            .ranges = vertices_.allocation_count(),
            .largest_free_vertex_block = vertices_.largest_free_block(),
            .largest_free_index_block = indices_.largest_free_block(),
        };
    }
} // namespace avis::visualizer::rendering
