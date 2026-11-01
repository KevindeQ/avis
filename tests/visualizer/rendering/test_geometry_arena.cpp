// The geometry arena (docs/tasks/11-renderer.md §11.3): the offset allocator, the vertex stream layout and the arena
// split are CPU-only; the arena's buffers need a device.

#include "helpers/recorded_assertions.h"
#include "helpers/rhi_fixture.h"
#include "rendering/geometry_arena.h"

#include "avis/graphics/error.h"
#include "avis/rhi/resources.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;

namespace
{
    // The live allocations, offset → size, checked against each other and the allocator
    struct allocation_tracker
    {
        std::map<std::uint64_t, std::uint64_t> live;

        // False when [offset, offset + size) overlaps a live allocation
        bool add(const std::uint64_t offset, const std::uint64_t size)
        {
            const auto next = live.lower_bound(offset);
            if (next != live.end() && next->first < offset + size)
            {
                return false;
            }
            if (next != live.begin())
            {
                const auto previous = std::prev(next);
                if (previous->first + previous->second > offset)
                {
                    return false;
                }
            }
            live.emplace(offset, size);
            return true;
        }

        std::uint64_t total() const
        {
            std::uint64_t sum = 0;
            for (const auto& [offset, size] : live)
            {
                sum += size;
            }
            return sum;
        }
    };
} // namespace

TEST_CASE("offset allocator: random allocations never overlap, and freeing everything coalesces", "[renderer][unit]")
{
    constexpr std::uint64_t capacity = std::uint64_t{ 1 } << 20;
    constexpr std::uint64_t alignment = 16;
    offset_allocator allocator{ capacity, alignment };
    allocation_tracker tracker;

    std::mt19937 random{ 1234 };
    std::uniform_int_distribution<std::uint64_t> sizes{ 1, 20'000 };
    std::bernoulli_distribution allocate_next{ 0.6 };
    std::size_t failures = 0;
    for (int operation = 0; operation < 20'000; ++operation)
    {
        if (tracker.live.empty() || allocate_next(random))
        {
            const std::uint64_t size = sizes(random);
            const std::optional<std::uint64_t> offset = allocator.allocate(size);
            if (!offset)
            {
                ++failures;
                CHECK(allocator.largest_free_block() < (size + alignment - 1) / alignment * alignment);
                continue;
            }
            const std::uint64_t rounded = allocator.allocation_size(*offset);
            CHECK(rounded >= size);
            CHECK(rounded % alignment == 0);
            CHECK(*offset % alignment == 0);
            CHECK(*offset + rounded <= capacity);
            REQUIRE(tracker.add(*offset, rounded));
        }
        else
        {
            std::uniform_int_distribution<std::size_t> pick{ 0, tracker.live.size() - 1 };
            const auto victim = std::next(tracker.live.begin(), static_cast<std::ptrdiff_t>(pick(random)));
            allocator.free(victim->first);
            tracker.live.erase(victim);
        }
        REQUIRE(allocator.used_bytes() == tracker.total());
        REQUIRE(allocator.allocation_count() == tracker.live.size());
    }
    CHECK(failures > 0); // the sequence filled the allocator at times

    for (const auto& [offset, size] : tracker.live)
    {
        allocator.free(offset);
    }
    CHECK(allocator.used_bytes() == 0);
    CHECK(allocator.allocation_count() == 0);
    CHECK(allocator.free_block_count() == 1);
    CHECK(allocator.largest_free_block() == capacity);
}

TEST_CASE("offset allocator: best fit takes the smallest block that fits", "[renderer][unit]")
{
    offset_allocator allocator{ 1000, 1 };
    const std::optional<std::uint64_t> large = allocator.allocate(300); // [0, 300)
    const std::optional<std::uint64_t> separator = allocator.allocate(10); // [300, 310)
    const std::optional<std::uint64_t> exact = allocator.allocate(100); // [310, 410)
    const std::optional<std::uint64_t> rest = allocator.allocate(590); // [410, 1000)
    REQUIRE(large == 0);
    REQUIRE(separator == 300);
    REQUIRE(exact == 310);
    REQUIRE(rest == 410);
    allocator.free(*large);
    allocator.free(*exact);
    REQUIRE(allocator.free_block_count() == 2);

    // First fit would split the 300-byte block at offset 0; best fit takes the exact 100-byte block
    CHECK(allocator.allocate(100) == 310);
    CHECK(allocator.largest_free_block() == 300);
    CHECK(allocator.free_block_count() == 1);
}

TEST_CASE("offset allocator: among equal blocks, the lowest offset wins", "[renderer][unit]")
{
    offset_allocator allocator{ 300, 1 };
    const auto first = allocator.allocate(100);
    const auto middle = allocator.allocate(100);
    const auto last = allocator.allocate(100);
    REQUIRE((first && middle && last));
    allocator.free(*last);
    allocator.free(*first);
    CHECK(allocator.allocate(100) == 0);
    CHECK(allocator.allocate(100) == 200);
}

TEST_CASE("offset allocator: freeing coalesces with both neighbours", "[renderer][unit]")
{
    offset_allocator allocator{ 4096, 16 };
    const auto a = allocator.allocate(1000);
    const auto b = allocator.allocate(1000);
    const auto c = allocator.allocate(1000);
    REQUIRE((a && b && c));
    CHECK(allocator.allocation_size(*a) == 1008); // rounded to 16
    CHECK(allocator.free_block_count() == 1); // the tail

    allocator.free(*b);
    CHECK(allocator.free_block_count() == 2);
    allocator.free(*a); // merges with b's block
    CHECK(allocator.free_block_count() == 2);
    CHECK(allocator.largest_free_block() == 2016);
    allocator.free(*c); // merges with the blocks on both sides
    CHECK(allocator.free_block_count() == 1);
    CHECK(allocator.largest_free_block() == 4096);
    CHECK(allocator.used_bytes() == 0);
}

TEST_CASE("offset allocator: misuse is a debug check", "[renderer][unit]")
{
    tests::recorded_assertions assertions;
    offset_allocator allocator{ 1024, 16 };
    CHECK_FALSE(allocator.allocate(0).has_value());
    CHECK_FALSE(allocator.allocate(2048).has_value()); // too large: no check, just no room
    allocator.free(512);
    CHECK(allocator.used_bytes() == 0);
#ifndef NDEBUG
    CHECK(assertions.count() == 2);
    CHECK(assertions.contains("0 bytes"));
    CHECK(assertions.contains("not allocated"));
#endif
}

TEST_CASE("vertex streams are laid out as arrays on 16-byte boundaries", "[renderer][unit]")
{
    SECTION("positions only")
    {
        const vertex_stream_layout layout =
            layout_vertex_streams(3, avis::flags<scene::vertex_attribute>{ avis::empty });
        CHECK(layout.offsets.position == 0);
        CHECK(layout.offsets.normal == vertex_stream_offsets::absent_stream);
        CHECK(layout.offsets.tangent == vertex_stream_offsets::absent_stream);
        CHECK(layout.offsets.uv0 == vertex_stream_offsets::absent_stream);
        CHECK(layout.offsets.color0 == vertex_stream_offsets::absent_stream);
        CHECK(layout.size == 48); // 36 bytes, padded
    }
    SECTION("positions, then each present attribute in order")
    {
        const vertex_stream_layout layout =
            layout_vertex_streams(3, scene::vertex_attribute::normal | scene::vertex_attribute::uv0);
        CHECK(layout.offsets.position == 0);
        CHECK(layout.offsets.normal == 48);
        CHECK(layout.offsets.uv0 == 64); // 48 + 12, padded
        CHECK(layout.offsets.tangent == vertex_stream_offsets::absent_stream);
        CHECK(layout.size == 80);
    }
    SECTION("every attribute")
    {
        const vertex_stream_layout layout = layout_vertex_streams(
            4,
            scene::vertex_attribute::normal | scene::vertex_attribute::tangent | scene::vertex_attribute::uv0 |
                scene::vertex_attribute::color0);
        CHECK(layout.offsets.position == 0);
        CHECK(layout.offsets.normal == 48);
        CHECK(layout.offsets.tangent == 64);
        CHECK(layout.offsets.uv0 == 80);
        CHECK(layout.offsets.color0 == 96);
        CHECK(layout.size == 112);
    }
}

TEST_CASE("the arena is split 80% vertices, 20% indices", "[renderer][unit]")
{
    const geometry_arena_capacity capacity = split_geometry_arena(768);
    constexpr std::uint64_t total = std::uint64_t{ 768 } << 20;
    CHECK(capacity.vertex_bytes + capacity.index_bytes <= total);
    CHECK(capacity.vertex_bytes + capacity.index_bytes > total - 32);
    CHECK(capacity.vertex_bytes % vertex_stream_alignment == 0);
    CHECK(capacity.index_bytes % index_alignment == 0);
    CHECK(capacity.vertex_bytes == total / 5 * 4 / vertex_stream_alignment * vertex_stream_alignment);
}

TEST_CASE("geometry arena: ranges come from both buffers, and a full buffer allocates nothing", "[renderer]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };

    SECTION("an empty or too large buffer is refused")
    {
        auto empty = geometry_arena::create(resources, { .vertex_bytes = 1024, .index_bytes = 0 });
        REQUIRE_FALSE(empty.has_value());
        CHECK(graphics::is_error(empty.error(), graphics::error_kind::invalid_argument));
        auto huge = geometry_arena::create(resources, { .vertex_bytes = std::uint64_t{ 4 } << 30, .index_bytes = 64 });
        REQUIRE_FALSE(huge.has_value());
        CHECK(graphics::is_error(huge.error(), graphics::error_kind::invalid_argument));
    }

    auto created = geometry_arena::create(resources, { .vertex_bytes = 1024, .index_bytes = 64 });
    REQUIRE(created.has_value());
    geometry_arena& arena = **created;
    CHECK(resources.description(arena.vertex_buffer()).size == 1024);
    CHECK(resources.description(arena.index_buffer()).size == 64);
    CHECK(resources.description(arena.index_buffer()).usage & rhi::buffer_usage::index);

    const vertex_stream_layout layout = layout_vertex_streams(3, scene::vertex_attribute::normal);
    const std::optional<geometry_range> first = arena.allocate(layout, 12);
    REQUIRE(first.has_value());
    CHECK(first->vertex_offset == 0);
    CHECK(first->vertex_size == layout.size);
    CHECK(first->streams.position == 0);
    CHECK(first->streams.normal == 48);
    CHECK(first->index_size == 12);

    const std::optional<geometry_range> second = arena.allocate(layout, 12);
    REQUIRE(second.has_value());
    CHECK(second->vertex_offset == layout.size);
    CHECK(second->streams.normal == second->vertex_offset + 48); // absolute
    CHECK(second->index_offset == 12);
    CHECK(second->first_index() == 3);

    SECTION("a range without indices takes no index space")
    {
        const std::optional<geometry_range> points = arena.allocate(64, 0);
        REQUIRE(points.has_value());
        CHECK(points->index_size == 0);
        CHECK(arena.statistics().index_used == 24);
        arena.free(*points);
    }
    SECTION("no room for the indices: the vertex part is given back")
    {
        const std::uint64_t vertices_before = arena.statistics().vertex_used;
        CHECK_FALSE(arena.allocate(64, 64).has_value());
        CHECK(arena.last_exhausted() == arena_exhausted::indices);
        CHECK(arena.statistics().vertex_used == vertices_before);
    }
    SECTION("no room for the vertices")
    {
        CHECK_FALSE(arena.allocate(1024, 4).has_value());
        CHECK(arena.last_exhausted() == arena_exhausted::vertices);
    }

    arena.free(*first);
    arena.free(*second);
    const geometry_arena_statistics statistics = arena.statistics();
    CHECK(statistics.ranges == 0);
    CHECK(statistics.vertex_used == 0);
    CHECK(statistics.index_used == 0);
    CHECK(statistics.largest_free_vertex_block == 1024);
}
