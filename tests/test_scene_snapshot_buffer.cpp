// snapshot_buffer: the double buffer between the simulation thread and the renderer (docs/tasks/10-scene-management.md
// §10.11, 08 §6.2, §9.8)

#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"

#include "avis/scene/snapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace avis;
using namespace std::chrono_literals;

namespace
{
    void write(scene::snapshot_buffer& buffer, const scene::object_id id)
    {
        scene::snapshot& target = buffer.begin_write();
        target.instances.push_back({ .id = id });
        buffer.publish();
    }
} // namespace

TEST_CASE("snapshot_buffer: acquire, publish and reuse", "[scene]")
{
    scene::snapshot_buffer buffer;
    CHECK(buffer.publish_sequence() == 0);

    SECTION("acquire before the first publish is empty")
    {
        const scene::snapshot_reference empty = buffer.acquire();
        CHECK_FALSE(empty);

        // An empty reference holds nothing, so the writer goes on
        write(buffer, 1);
        CHECK(buffer.publish_sequence() == 1);
    }

    SECTION("between publishes the reader sees the same snapshot")
    {
        write(buffer, 1);
        const scene::snapshot* first = nullptr;
        {
            const scene::snapshot_reference front = buffer.acquire();
            REQUIRE(front);
            CHECK(front->publish_sequence == 1);
            REQUIRE(front->instances.size() == 1);
            CHECK(front->instances[0].id == 1);
            first = &*front;
        }
        {
            const scene::snapshot_reference again = buffer.acquire();
            CHECK(&*again == first);
            CHECK(again->publish_sequence == 1);
        }

        write(buffer, 2);
        const scene::snapshot_reference next = buffer.acquire();
        CHECK(&*next != first);
        CHECK(next->publish_sequence == 2);
        CHECK(next->instances[0].id == 2);
    }

    SECTION("begin_write clears the slot and keeps its capacity")
    {
        scene::snapshot& target = buffer.begin_write();
        target.instances.resize(64);
        const std::size_t capacity = target.instances.capacity();
        buffer.publish();
        write(buffer, 2);

        scene::snapshot& reused = buffer.begin_write();
        CHECK(&reused == &target);
        CHECK(reused.instances.empty());
        CHECK(reused.instances.capacity() == capacity);
        buffer.publish();
    }

    SECTION("a reference moves")
    {
        write(buffer, 1);
        scene::snapshot_reference first = buffer.acquire();
        scene::snapshot_reference moved = std::move(first);
        CHECK_FALSE(first);
        REQUIRE(moved);
        CHECK(moved->instances[0].id == 1);

        moved = scene::snapshot_reference{};
        CHECK_FALSE(moved);
        CHECK(buffer.acquire()); // released, so a new one is allowed
    }
}

TEST_CASE("snapshot_buffer: the reader's hold blocks only the second begin_write", "[scene]")
{
    scene::snapshot_buffer buffer;
    write(buffer, 1);

    std::atomic<bool> second_write_started = false;
    std::atomic<bool> second_write_done = false;
    std::thread writer;
    {
        const scene::snapshot_reference held = buffer.acquire();
        REQUIRE(held);

        // Across one publish: the writer fills the other slot without waiting
        write(buffer, 2);
        CHECK(held->instances[0].id == 1);

        // Across two: the next begin_write needs the held slot and waits
        writer = std::thread{ [&]
                              {
                                  second_write_started = true;
                                  write(buffer, 3);
                                  second_write_done = true;
                              } };
        while (!second_write_started)
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(50ms);
        CHECK_FALSE(second_write_done);
        CHECK(held->instances[0].id == 1); // untouched while held
        CHECK(held->publish_sequence == 1);
    }
    writer.join();
    CHECK(second_write_done);
    CHECK(buffer.publish_sequence() == 3);
    CHECK(buffer.acquire()->instances[0].id == 3);
}

TEST_CASE("snapshot_buffer: misuse is checked", "[scene]")
{
    scene::snapshot_buffer buffer;
    tests::recorded_assertions assertions;
    tests::log_capture errors{ "scene" };

    SECTION("a second concurrent acquire")
    {
        write(buffer, 1);
        const scene::snapshot_reference first = buffer.acquire();
        const scene::snapshot_reference second = buffer.acquire();
        CHECK(first);
        CHECK_FALSE(second);
#ifndef NDEBUG
        CHECK(assertions.count() == 1);
        CHECK(assertions.contains("snapshot_reference is still alive"));
#endif
    }

    SECTION("publish without begin_write")
    {
        buffer.publish();
        CHECK(buffer.publish_sequence() == 0);
#ifndef NDEBUG
        CHECK(assertions.contains("publish: no begin_write before it"));
#endif
    }

    SECTION("begin_write twice")
    {
        buffer.begin_write();
        buffer.begin_write();
        buffer.publish();
#ifndef NDEBUG
        CHECK(assertions.contains("begin_write: called again before publish"));
#endif
    }
}

// 08 §10: a writer at 1 kHz and a reader at 240 Hz for 5 s. Every instance of a snapshot carries the sequence it is
// published with, so a torn read (a slot rewritten while read) shows as mixed ids.
TEST_CASE("snapshot_buffer: no torn reads under a 1 kHz writer and a 240 Hz reader", "[scene]")
{
    scene::snapshot_buffer buffer;
    constexpr auto duration = 5s;
    constexpr std::size_t instances_per_snapshot = 256;

    std::atomic<bool> stop = false;
    std::atomic<std::uint64_t> torn = 0;
    std::atomic<std::uint64_t> backwards = 0;
    std::atomic<std::uint64_t> reads = 0;

    std::thread reader{ [&]
                        {
                            std::uint64_t last_sequence = 0;
                            auto next = std::chrono::steady_clock::now();
                            while (!stop)
                            {
                                {
                                    const scene::snapshot_reference front = buffer.acquire();
                                    if (front)
                                    {
                                        const std::uint64_t sequence = front->publish_sequence;
                                        if (sequence < last_sequence)
                                        {
                                            ++backwards;
                                        }
                                        last_sequence = sequence;
                                        if (front->instances.size() != instances_per_snapshot + sequence % 8)
                                        {
                                            ++torn;
                                        }
                                        for (const scene::render_instance& instance : front->instances)
                                        {
                                            if (instance.id != sequence)
                                            {
                                                ++torn;
                                                break;
                                            }
                                        }
                                        ++reads;
                                    }
                                }
                                next += std::chrono::microseconds{ 4167 };
                                std::this_thread::sleep_until(next);
                            }
                        } };

    const auto end = std::chrono::steady_clock::now() + duration;
    auto next = std::chrono::steady_clock::now();
    std::uint64_t writes = 0;
    while (std::chrono::steady_clock::now() < end)
    {
        const std::uint64_t sequence = buffer.publish_sequence() + 1;
        scene::snapshot& target = buffer.begin_write();
        target.instances.resize(instances_per_snapshot + sequence % 8); // the size changes too
        for (scene::render_instance& instance : target.instances)
        {
            instance.id = sequence;
        }
        buffer.publish();
        ++writes;

        next += 1ms;
        std::this_thread::sleep_until(next);
    }
    stop = true;
    reader.join();

    INFO("writes: " << writes << ", reads: " << reads.load());
    CHECK(torn == 0);
    CHECK(backwards == 0);
    CHECK(buffer.publish_sequence() == writes);
    CHECK(writes > 500);
    CHECK(reads > 100);
}
