#include "avis/graphics/transient_resource_pool.h"

#include "avis/detail/check.h"
#include "avis/graphics/error.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace avis::graphics
{
    namespace
    {
        constexpr std::string_view logger_name = "graphics";

        void combine(std::size_t& seed, const std::uint64_t value) noexcept
        {
            seed ^= std::hash<std::uint64_t>{}(value) + 0x9E37'79B9'7F4A'7C15ull + (seed << 6) + (seed >> 2);
        }

        // The debug name is left out: it changes nothing on the GPU
        std::size_t hash_of(const rhi::texture_description& description) noexcept
        {
            std::size_t seed = 0x7E47;
            combine(seed, static_cast<std::uint64_t>(description.dimension));
            combine(seed, description.width);
            combine(seed, description.height);
            combine(seed, description.depth_or_layers);
            combine(seed, description.mip_levels);
            combine(seed, static_cast<std::uint64_t>(description.format));
            combine(seed, description.usage.underlying_value());
            combine(seed, static_cast<std::uint64_t>(description.initial_layout));
            combine(seed, description.optimized_clear.has_value() ? 1 : 0);
            if (description.optimized_clear.has_value())
            {
                for (const float channel : description.optimized_clear->color)
                {
                    combine(seed, std::bit_cast<std::uint32_t>(channel));
                }
                combine(seed, std::bit_cast<std::uint32_t>(description.optimized_clear->depth));
                combine(seed, description.optimized_clear->stencil);
            }
            return seed;
        }

        std::size_t hash_of(const rhi::buffer_description& description) noexcept
        {
            std::size_t seed = 0xB0FF;
            combine(seed, description.size);
            combine(seed, description.structure_stride);
            combine(seed, description.usage.underlying_value());
            combine(seed, static_cast<std::uint64_t>(description.memory));
            return seed;
        }

        bool same_clear(const std::optional<rhi::clear_value>& a, const std::optional<rhi::clear_value>& b) noexcept
        {
            if (a.has_value() != b.has_value())
            {
                return false;
            }
            return !a.has_value() || (a->color == b->color && a->depth == b->depth && a->stencil == b->stencil);
        }

        bool same(const rhi::texture_description& a, const rhi::texture_description& b) noexcept
        {
            return a.dimension == b.dimension && a.width == b.width && a.height == b.height &&
                   a.depth_or_layers == b.depth_or_layers && a.mip_levels == b.mip_levels && a.format == b.format &&
                   a.usage == b.usage && a.initial_layout == b.initial_layout &&
                   same_clear(a.optimized_clear, b.optimized_clear);
        }

        bool same(const rhi::buffer_description& a, const rhi::buffer_description& b) noexcept
        {
            return a.size == b.size && a.structure_stride == b.structure_stride && a.usage == b.usage &&
                   a.memory == b.memory;
        }

        template<typename Description>
        Description without_name(Description description) noexcept
        {
            description.debug_name = {};
            return description;
        }

        constexpr std::uint64_t round_up(const std::uint64_t size, const std::uint64_t granularity) noexcept
        {
            return (size + granularity - 1) / granularity * granularity;
        }
    } // namespace

    std::size_t transient_resource_pool::cache_key_hash::operator()(const cache_key& key) const noexcept
    {
        std::size_t seed = key.description_hash;
        combine(seed, key.offset);
        combine(seed, key.texture ? 1 : 0);
        return seed;
    }

    transient_resource_pool::transient_resource_pool(
        const rhi::memory memory, const rhi::resources resources, const std::uint32_t frames_in_flight) :
        memory_{ memory },
        resources_{ resources },
        contexts_(frames_in_flight)
    {}

    transient_resource_pool::~transient_resource_pool()
    {
        for (frame_context& context : contexts_)
        {
            destroy_cache(context);
            if (context.heap.is_valid())
            {
                memory_.destroy(context.heap);
            }
        }
    }

    rhi::allocation_requirements transient_resource_pool::requirements(const rhi::texture_description& description)
    {
        const std::size_t hash = hash_of(description);
        const auto found = texture_requirements_.find(hash);
        if (found != texture_requirements_.end() && same(found->second.description, description))
        {
            return found->second.requirements;
        }
        const rhi::allocation_requirements requirements = memory_.requirements(description);
        texture_requirements_.insert_or_assign(
            hash, texture_requirements{ .description = without_name(description), .requirements = requirements });
        return requirements;
    }

    rhi::allocation_requirements transient_resource_pool::requirements(const rhi::buffer_description& description)
    {
        const std::size_t hash = hash_of(description);
        const auto found = buffer_requirements_.find(hash);
        if (found != buffer_requirements_.end() && same(found->second.description, description))
        {
            return found->second.requirements;
        }
        const rhi::allocation_requirements requirements = memory_.requirements(description);
        buffer_requirements_.insert_or_assign(
            hash, buffer_requirements{ .description = without_name(description), .requirements = requirements });
        return requirements;
    }

    result<void> transient_resource_pool::prepare(const std::uint32_t context_index, const std::uint64_t heap_size)
    {
        frame_context* const entry = context(context_index);
        if (entry == nullptr)
        {
            return std::unexpected(make_error(error_kind::invalid_argument, "transient_resource_pool: bad context"));
        }
        ++frame_;
        frame_context& current = *entry;

        // Resources nobody acquired for a while
        std::erase_if(
            current.cache,
            [&](auto& item)
            {
                if (frame_ - item.second.last_used <= retention_frames)
                {
                    return false;
                }
                destroy_resource(item.second);
                return true;
            });

        current.high_water_mark = std::max(current.high_water_mark, heap_size);
        const std::uint64_t needed = round_up(heap_size, heap_granularity);
        if (needed > current.size)
        {
            current.frames_below = 0;
            current.recent_peak = 0;
            return replace_heap(context_index, current, needed);
        }
        if (needed == current.size)
        {
            current.frames_below = 0;
            current.recent_peak = 0;
            return {};
        }

        current.recent_peak = std::max(current.recent_peak, heap_size);
        if (++current.frames_below < retention_frames)
        {
            return {};
        }
        const std::uint64_t target = round_up(current.recent_peak, heap_granularity);
        current.frames_below = 0;
        current.recent_peak = 0;
        return replace_heap(context_index, current, target);
    }

    result<rhi::texture_handle> transient_resource_pool::acquire_texture(
        const std::uint32_t context_index, const rhi::texture_description& description, const std::uint64_t offset)
    {
        frame_context* const entry = context(context_index);
        if (!AVIS_CHECK(
                logger_name,
                entry != nullptr && entry->heap.is_valid(),
                "transient_resource_pool::acquire_texture: no heap; call prepare first"))
        {
            return std::unexpected(make_error(error_kind::invalid_argument, "transient_resource_pool: no heap"));
        }

        const cache_key key{ .description_hash = hash_of(description), .offset = offset, .texture = true };
        const auto found = entry->cache.find(key);
        if (found != entry->cache.end())
        {
            if (same(found->second.texture, description))
            {
                found->second.last_used = frame_;
                return found->second.texture_handle;
            }
            destroy_resource(found->second); // a hash collision: the newer description wins
            entry->cache.erase(found);
        }

        auto texture = memory_.create_placed_texture(entry->heap, offset, description);
        if (!texture)
        {
            return std::unexpected(std::move(texture.error()));
        }
        ++statistics_.textures_created;
        entry->cache.emplace(
            key,
            cached_resource{ .texture = without_name(description), .texture_handle = *texture, .last_used = frame_ });
        return *texture;
    }

    result<rhi::buffer_handle> transient_resource_pool::acquire_buffer(
        const std::uint32_t context_index, const rhi::buffer_description& description, const std::uint64_t offset)
    {
        frame_context* const entry = context(context_index);
        if (!AVIS_CHECK(
                logger_name,
                entry != nullptr && entry->heap.is_valid(),
                "transient_resource_pool::acquire_buffer: no heap; call prepare first"))
        {
            return std::unexpected(make_error(error_kind::invalid_argument, "transient_resource_pool: no heap"));
        }

        const cache_key key{ .description_hash = hash_of(description), .offset = offset, .texture = false };
        const auto found = entry->cache.find(key);
        if (found != entry->cache.end())
        {
            if (same(found->second.buffer, description))
            {
                found->second.last_used = frame_;
                return found->second.buffer_handle;
            }
            destroy_resource(found->second);
            entry->cache.erase(found);
        }

        auto buffer = memory_.create_placed_buffer(entry->heap, offset, description);
        if (!buffer)
        {
            return std::unexpected(std::move(buffer.error()));
        }
        ++statistics_.buffers_created;
        entry->cache.emplace(
            key, cached_resource{ .buffer = without_name(description), .buffer_handle = *buffer, .last_used = frame_ });
        return *buffer;
    }

    rhi::heap_handle transient_resource_pool::heap(const std::uint32_t context_index) const noexcept
    {
        const frame_context* const entry = context(context_index);
        return entry != nullptr ? entry->heap : rhi::heap_handle{};
    }

    std::uint64_t transient_resource_pool::heap_size(const std::uint32_t context_index) const noexcept
    {
        const frame_context* const entry = context(context_index);
        return entry != nullptr ? entry->size : 0;
    }

    std::uint64_t transient_resource_pool::high_water_mark(const std::uint32_t context_index) const noexcept
    {
        const frame_context* const entry = context(context_index);
        return entry != nullptr ? entry->high_water_mark : 0;
    }

    transient_resource_pool::frame_context* transient_resource_pool::context(const std::uint32_t context_index) noexcept
    {
        return AVIS_CHECK(
                   logger_name,
                   context_index < contexts_.size(),
                   "transient_resource_pool: context index out of range") ?
                   &contexts_[context_index] :
                   nullptr;
    }

    const transient_resource_pool::frame_context*
        transient_resource_pool::context(const std::uint32_t context_index) const noexcept
    {
        return const_cast<transient_resource_pool*>(this)->context(context_index);
    }

    void transient_resource_pool::destroy_resource(cached_resource& resource) noexcept
    {
        if (resource.texture_handle.is_valid())
        {
            resources_.destroy(resource.texture_handle);
        }
        if (resource.buffer_handle.is_valid())
        {
            resources_.destroy(resource.buffer_handle);
        }
        ++statistics_.resources_destroyed;
    }

    void transient_resource_pool::destroy_cache(frame_context& context) noexcept
    {
        for (auto& item : context.cache)
        {
            destroy_resource(item.second);
        }
        context.cache.clear();
    }

    result<void> transient_resource_pool::replace_heap(
        const std::uint32_t context_index, frame_context& context, const std::uint64_t size)
    {
        // The placed resources go first: a heap may only be destroyed once nothing placed in it is alive
        destroy_cache(context);
        if (context.heap.is_valid())
        {
            memory_.destroy(context.heap);
            ++statistics_.heaps_destroyed;
        }
        context.heap = {};
        context.size = 0;
        if (size == 0)
        {
            return {};
        }

        const std::string name = std::format("render graph transient heap {}", context_index);
        auto heap = memory_.create_heap({ .size = size, .kind = rhi::heap_kind::all_resources, .debug_name = name });
        if (!heap)
        {
            return std::unexpected(std::move(heap.error()));
        }
        ++statistics_.heaps_created;
        context.heap = *heap;
        context.size = size;
        return {};
    }
} // namespace avis::graphics
