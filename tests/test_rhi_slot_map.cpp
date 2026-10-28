// The slot map behind public handles and the descriptor free list (docs/tasks/06-rhi.md §6.6). No device.

#include "helpers/recorded_assertions.h"

#include "avis/rhi/d3d12/descriptor_heaps.h"
#include "avis/rhi/d3d12/slot_map.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using avis::rhi::d3d12::handle_bits;
using avis::rhi::d3d12::index_free_list;
using avis::rhi::d3d12::slot_map;

TEST_CASE("slot_map: insert, find and extract", "[rhi][unit]")
{
    slot_map<std::string> map;

    const auto inserted = map.insert("first");
    REQUIRE(inserted.has_value());
    const handle_bits first = *inserted;
    CHECK((first.generation & 1) == 1);
    REQUIRE(map.find(first) != nullptr);
    CHECK(*map.find(first) == "first");

    const std::optional<std::string> extracted = map.extract(first);
    REQUIRE(extracted.has_value());
    CHECK(*extracted == "first");
    CHECK(map.find(first) == nullptr);
    CHECK_FALSE(map.extract(first).has_value());

    SECTION("a reused slot gets a new generation")
    {
        const auto again = map.insert("second");
        REQUIRE(again.has_value());
        CHECK(again->index == first.index);
        CHECK(again->generation != first.generation);
        CHECK(map.find(first) == nullptr);
        CHECK(*map.find(*again) == "second");
    }

    SECTION("a default handle finds nothing")
    {
        CHECK(map.find(handle_bits{}) == nullptr);
    }
}

TEST_CASE("slot_map: any handle bits are safe to look up", "[rhi][unit]")
{
    const avis::tests::recorded_assertions assertions;
    slot_map<std::uint64_t> map;
    const handle_bits alive = *map.insert(std::uint64_t{ 7 });

    // An even generation is a free slot
    CHECK(map.find(map.make_bits(alive.index, alive.generation + 1)) == nullptr);
    // A chunk that isn't allocated yet, and the last chunk
    CHECK(map.find(map.make_bits(5 * slot_map<std::uint64_t>::chunk_size, 1)) == nullptr);
    CHECK(
        map.find(map.make_bits(slot_map<std::uint64_t>::chunk_size * slot_map<std::uint64_t>::chunk_count - 1, 1)) ==
        nullptr);
    // A slot past the end of an allocated chunk's used part
    CHECK(map.find(map.make_bits(alive.index + 1, 1)) == nullptr);
    CHECK(assertions.count() == 0);

    // Bits of another map, or garbage: a foreign instance tag
    slot_map<std::uint64_t> other;
    const handle_bits foreign = *other.insert(std::uint64_t{ 8 });
    CHECK(map.find(foreign) == nullptr);
    CHECK(map.find(handle_bits{ .index = 0x000F'FFFF, .generation = 1 }) == nullptr); // tag 0: never a map's
#ifndef NDEBUG
    CHECK(assertions.count() == 2);
    CHECK(assertions.contains("another"));
#endif
}

TEST_CASE("slot_map: record addresses stay stable across 1M inserts", "[rhi][unit]")
{
    auto map = std::make_unique<slot_map<std::uint64_t>>();
    const handle_bits first = *map->insert(std::uint64_t{ 42 });
    const std::uint64_t* const address = map->find(first);

    for (std::uint64_t value = 0; value < 1'000'000; ++value)
    {
        REQUIRE(map->insert(value).has_value());
    }
    CHECK(map->find(first) == address);
    CHECK(*address == 42);
}

TEST_CASE("slot_map: lock-free find runs next to insert and extract", "[rhi][unit]")
{
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        slot_map<std::uint64_t> map;
        std::vector<handle_bits> stable;
        for (std::uint64_t value = 0; value < 64; ++value)
        {
            stable.push_back(*map.insert(value));
        }

        std::atomic<bool> stop{ false };
        std::atomic<int> wrong{ 0 };
        std::vector<std::thread> threads;
        for (int finder = 0; finder < 8; ++finder)
        {
            threads.emplace_back(
                [&]
                {
                    while (!stop.load())
                    {
                        for (std::uint64_t value = 0; value < stable.size(); ++value)
                        {
                            const std::uint64_t* const found = map.find(stable[value]);
                            if (found == nullptr || *found != value)
                            {
                                ++wrong;
                            }
                        }
                    }
                });
        }
        std::vector<std::thread> writers;
        for (int writer = 0; writer < 2; ++writer)
        {
            writers.emplace_back(
                [&]
                {
                    for (int round = 0; round < 2'000; ++round)
                    {
                        const auto inserted = map.insert(std::uint64_t{ 1'000'000 });
                        if (!inserted || !map.extract(*inserted))
                        {
                            ++wrong;
                        }
                    }
                });
        }
        for (std::thread& writer : writers)
        {
            writer.join();
        }
        stop = true;
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        REQUIRE(wrong == 0);
    }
}

TEST_CASE("index_free_list: concurrent pop and push keep every index unique", "[rhi][unit]")
{
    constexpr std::uint32_t capacity = 1024;
    index_free_list list;
    list.initialise(0, capacity);
    CHECK(list.count() == capacity);

    auto in_use = std::make_unique<std::atomic<int>[]>(capacity);
    std::atomic<int> duplicates{ 0 };
    std::atomic<int> empty{ 0 };
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 16; ++thread)
    {
        threads.emplace_back(
            [&]
            {
                for (int pair = 0; pair < 100'000; ++pair)
                {
                    const std::optional<std::uint32_t> index = list.pop();
                    if (!index)
                    {
                        ++empty;
                        continue;
                    }
                    if (in_use[*index].exchange(1) != 0)
                    {
                        ++duplicates; // the same index handed out twice: ABA
                    }
                    in_use[*index].store(0);
                    list.push(*index);
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    CHECK(duplicates == 0);
    CHECK(empty == 0);
    CHECK(list.count() == capacity);
}

TEST_CASE("index_free_list: indices below the first are never handed out", "[rhi][unit]")
{
    index_free_list list;
    list.initialise(1, 4);
    CHECK(list.pop() == 1u);
    CHECK(list.pop() == 2u);
    CHECK(list.pop() == 3u);
    CHECK_FALSE(list.pop().has_value());
    list.push(2);
    CHECK(list.pop() == 2u);
}
