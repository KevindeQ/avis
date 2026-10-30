// The render graph's per-frame linear arena (docs/tasks/09-render-graph.md §9.1). No device.

#include "helpers/allocation_failure.h"
#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"

#include "avis/graphics/linear_arena.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using avis::graphics::arena_allocator;
using avis::graphics::linear_arena;
using avis::graphics::linear_arena_configuration;

namespace
{
    // Appends its id to a shared log when destroyed
    struct destruction_logger
    {
        std::vector<int>* log;
        int id;

        ~destruction_logger()
        {
            log->push_back(id);
        }
    };

    struct trivial_pair
    {
        std::uint32_t first;
        std::uint32_t second;
    };

    bool is_aligned(const void* pointer, const std::size_t alignment)
    {
        return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
    }
} // namespace

TEST_CASE("linear_arena: alignments from 1 to 4096 are honoured and allocations don't overlap", "[graphics][unit]")
{
    linear_arena arena{ { .block_size = 64 * 1024 } };

    struct range
    {
        std::uintptr_t begin;
        std::uintptr_t end;
    };
    std::vector<range> ranges;

    for (int round = 0; round < 8; ++round)
    {
        for (std::size_t alignment = 1; alignment <= 4096; alignment *= 2)
        {
            const std::size_t size = 1 + (alignment * 3 + static_cast<std::size_t>(round) * 7) % 300;
            void* const memory = arena.allocate(size, alignment);
            REQUIRE(memory != nullptr);
            CHECK(is_aligned(memory, alignment));
            std::fill_n(static_cast<std::byte*>(memory), size, std::byte{ 0xCD });
            const auto begin = reinterpret_cast<std::uintptr_t>(memory);
            ranges.push_back({ begin, begin + size });
        }
    }

    std::ranges::sort(ranges, {}, &range::begin);
    for (std::size_t index = 1; index < ranges.size(); ++index)
    {
        CHECK(ranges[index - 1].end <= ranges[index].begin);
    }
}

TEST_CASE("linear_arena: a request larger than half a block gets its own block, freed by reset", "[graphics][unit]")
{
    constexpr std::size_t block_size = 16 * 1024;
    linear_arena arena{ { .block_size = block_size } };

    // One regular block in use
    arena.allocate(64);
    const std::size_t reserved_before = arena.bytes_reserved();

    void* const large = arena.allocate(3 * block_size, 256);
    CHECK(is_aligned(large, 256));
    std::fill_n(static_cast<std::byte*>(large), 3 * block_size, std::byte{ 0xAB });
    CHECK(arena.bytes_reserved() >= reserved_before + 3 * block_size);
    CHECK(arena.bytes_used() >= 64 + 3 * block_size);

    // The regular block continues after the dedicated one
    void* const small = arena.allocate(64);
    CHECK(arena.bytes_reserved() < reserved_before + 4 * block_size);
    CHECK(
        (static_cast<std::byte*>(small) < static_cast<std::byte*>(large) ||
         static_cast<std::byte*>(small) >= static_cast<std::byte*>(large) + 3 * block_size));

    arena.reset();
    CHECK(arena.bytes_reserved() == reserved_before);
    CHECK(arena.bytes_used() == 0);
}

TEST_CASE("linear_arena: destructors run in reverse creation order", "[graphics][unit]")
{
    std::vector<int> log;

    SECTION("on reset")
    {
        linear_arena arena;
        for (int id = 0; id < 5; ++id)
        {
            arena.create<destruction_logger>(&log, id);
        }
        CHECK(log.empty());

        arena.reset();
        CHECK(log == std::vector<int>{ 4, 3, 2, 1, 0 });

        // Destroyed objects aren't destroyed again
        arena.reset();
        CHECK(log.size() == 5);
    }

    SECTION("on destruction")
    {
        {
            linear_arena arena;
            for (int id = 0; id < 3; ++id)
            {
                arena.create<destruction_logger>(&log, id);
            }
        }
        CHECK(log == std::vector<int>{ 2, 1, 0 });
    }

    SECTION("across blocks")
    {
        // Small blocks, so the objects and their records span many blocks
        linear_arena arena{ { .block_size = 256 } };
        for (int id = 0; id < 100; ++id)
        {
            arena.create<destruction_logger>(&log, id);
        }
        arena.reset();
        REQUIRE(log.size() == 100);
        CHECK(std::ranges::is_sorted(log, std::ranges::greater{}));
    }
}

TEST_CASE("linear_arena: objects with non-trivial members are destroyed on reset", "[graphics][unit]")
{
    const auto shared = std::make_shared<int>(42);
    linear_arena arena;

    struct pass_data
    {
        std::string name;
        std::shared_ptr<int> payload;
    };
    pass_data& data = arena.create<pass_data>(std::string(100, 'x'), shared);
    CHECK(data.name.size() == 100);
    CHECK(shared.use_count() == 2);

    auto callable = [captured = shared] { return *captured; };
    auto& stored = arena.create<decltype(callable)>(std::move(callable));
    CHECK(stored() == 42);
    CHECK(shared.use_count() == 3);

    arena.reset();
    CHECK(shared.use_count() == 1);
}

TEST_CASE("linear_arena: trivially destructible types register no destructor record", "[graphics][unit]")
{
    linear_arena arena;

    // The block data is max_align_t-aligned, so the first allocation needs no padding
    arena.create<trivial_pair>(1u, 2u);
    CHECK(arena.bytes_used() == sizeof(trivial_pair));
    arena.create<std::uint64_t>(std::uint64_t{ 7 });
    CHECK(arena.bytes_used() == sizeof(trivial_pair) + sizeof(std::uint64_t));

    arena.reset();
    std::vector<int> log;
    arena.create<destruction_logger>(&log, 0);
    CHECK(arena.bytes_used() > sizeof(destruction_logger));
}

TEST_CASE("linear_arena: allocate_array returns a span of the requested size", "[graphics][unit]")
{
    linear_arena arena;

    const std::span<std::uint32_t> values = arena.allocate_array<std::uint32_t>(1000);
    REQUIRE(values.size() == 1000);
    CHECK(is_aligned(values.data(), alignof(std::uint32_t)));
    for (std::uint32_t index = 0; index < values.size(); ++index)
    {
        values[index] = index * 3;
    }
    CHECK(values[999] == 2997);

    CHECK(arena.allocate_array<trivial_pair>(0).empty());
}

TEST_CASE("linear_arena: a steady-state frame allocates nothing from the OS", "[graphics][unit]")
{
    linear_arena arena{ { .block_size = 4096, .maximum_retained_blocks = 16 } };
    std::vector<int> log;
    log.reserve(64);

    // About 8 KiB per frame, over a few 4 KiB blocks
    const auto frame = [&]
    {
        for (int index = 0; index < 32; ++index)
        {
            arena.allocate(200, 16);
            arena.create<trivial_pair>(1u, 2u);
        }
        arena.create<destruction_logger>(&log, 0);
        arena.allocate_array<std::uint16_t>(500);
    };

    frame();
    arena.reset();
    log.clear();
    const std::size_t reserved = arena.bytes_reserved();
    CHECK(reserved >= 2 * 4096);

    for (int index = 0; index < 100; ++index)
    {
        bool allocated = false;
        {
            // Arms a failure for the next operator new; checked outside the scope, where Catch may allocate
            const avis::tests::scoped_allocation_failure no_allocation;
            frame();
            allocated = no_allocation.triggered();
        }
        CHECK_FALSE(allocated);
        arena.reset();
        CHECK(arena.bytes_reserved() == reserved);
        log.clear();
    }
}

TEST_CASE("linear_arena: reset releases blocks above the retention limit", "[graphics][unit]")
{
    constexpr std::size_t block_size = 1024;
    linear_arena arena{ { .block_size = block_size, .maximum_retained_blocks = 2 } };

    // Two allocations of 400 bytes fit in one block
    for (int index = 0; index < 20; ++index)
    {
        arena.allocate(400);
    }
    const std::size_t reserved_with_ten = arena.bytes_reserved();
    CHECK(reserved_with_ten >= 10 * block_size);

    arena.reset();
    const std::size_t reserved_with_two = arena.bytes_reserved();
    CHECK(reserved_with_two >= 2 * block_size);
    CHECK(reserved_with_two < 3 * block_size);
    CHECK(reserved_with_two * 5 == reserved_with_ten);

    // The retained blocks are reused: four allocations need no new block
    for (int index = 0; index < 4; ++index)
    {
        arena.allocate(400);
    }
    CHECK(arena.bytes_reserved() == reserved_with_two);

    SECTION("a limit of 0 releases every block")
    {
        linear_arena empty{ { .block_size = block_size, .maximum_retained_blocks = 0 } };
        empty.allocate(16);
        CHECK(empty.bytes_reserved() > 0);
        empty.reset();
        CHECK(empty.bytes_reserved() == 0);
        CHECK(empty.allocate(16) != nullptr);
    }
}

TEST_CASE("linear_arena: std::vector with arena_allocator grows to 100,000 elements", "[graphics][unit]")
{
    linear_arena arena;
    std::vector<int, arena_allocator<int>> values{ arena_allocator<int>{ arena } };
    for (int index = 0; index < 100'000; ++index)
    {
        values.push_back(index);
    }
    REQUIRE(values.size() == 100'000);
    CHECK(values.front() == 0);
    CHECK(values[54'321] == 54'321);
    CHECK(values.back() == 99'999);
    CHECK(arena.bytes_used() >= 100'000 * sizeof(int));

    // Allocators of one arena compare equal across value types, of two arenas not
    linear_arena other;
    CHECK(arena_allocator<int>{ arena } == arena_allocator<double>{ arena });
    CHECK_FALSE(arena_allocator<int>{ arena } == arena_allocator<int>{ other });
    CHECK(arena_allocator<double>{ arena_allocator<int>{ arena } } == arena_allocator<int>{ arena });
}

TEST_CASE("linear_arena: an alignment that isn't a power of two fires the check", "[graphics][unit]")
{
    const avis::tests::recorded_assertions assertions;
    const avis::tests::log_capture capture{ "graphics" };
    linear_arena arena;

    void* const memory = arena.allocate(24, 24);
    REQUIRE(memory != nullptr);
    CHECK(is_aligned(memory, 32)); // rounded up in release builds, and after the check in debug ones

#ifndef NDEBUG
    CHECK(assertions.contains("alignment isn't a power of two"));
#else
    CHECK(assertions.count() == 0);
#endif
}
