// Transient objects: linear upload allocators, descriptor ranges and timestamp query sets (docs/tasks/06-rhi.md §6.9).
// Needs a D3D12 device (WARP is enough).

#include "helpers/recorded_assertions.h"
#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"

#include "avis/rhi/completion_driver.h"
#include "avis/rhi/error.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/transient.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace avis;

namespace
{
    std::size_t count_lines(const tests::test_rhi& hardware_interface, const std::string& text)
    {
        std::size_t lines = 0;
        for (const std::string& line : hardware_interface.log().lines())
        {
            lines += line.contains(text) ? 1 : 0;
        }
        return lines;
    }

    rhi::render_hardware_interface_configuration small_transient_region()
    {
        rhi::render_hardware_interface_configuration configuration;
        configuration.descriptor_heaps.transient_descriptor_count = 8;
        return configuration;
    }
} // namespace

TEST_CASE("linear_upload_allocator: concurrent allocations don't overlap", "[rhi]")
{
    constexpr int threads = 16;
    constexpr int per_thread = 10'000;
    constexpr std::size_t size = 16;

    tests::test_rhi hardware_interface;
    rhi::transient_objects transient{ *hardware_interface };
    auto allocator = transient.create_linear_upload_allocator(4 << 20, "uploads");
    REQUIRE(allocator.has_value());
    CHECK(allocator->capacity() == 4 << 20);

    std::vector<std::vector<std::uint64_t>> addresses(threads);
    std::vector<std::thread> workers;
    for (int thread = 0; thread < threads; ++thread)
    {
        workers.emplace_back(
            [&, thread]
            {
                addresses[thread].reserve(per_thread);
                for (int index = 0; index < per_thread; ++index)
                {
                    const auto allocation = allocator->allocate(size, 16);
                    if (allocation)
                    {
                        addresses[thread].push_back(allocation->gpu.value);
                    }
                }
            });
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }

    std::vector<std::uint64_t> all;
    for (const auto& list : addresses)
    {
        all.insert(all.end(), list.begin(), list.end());
    }
    REQUIRE(all.size() == std::size_t{ threads } * per_thread);
    std::ranges::sort(all);
    bool overlapping = false;
    bool misaligned = false;
    for (std::size_t index = 0; index < all.size(); ++index)
    {
        misaligned = misaligned || all[index] % 16 != 0;
        overlapping = overlapping || (index > 0 && all[index] - all[index - 1] < size);
    }
    CHECK_FALSE(overlapping);
    CHECK_FALSE(misaligned);
    CHECK(allocator->high_water_mark() == all.size() * size);
}

TEST_CASE("linear_upload_allocator: exhaustion fails and logs once per reset cycle", "[rhi]")
{
    tests::test_rhi hardware_interface;
    rhi::transient_objects transient{ *hardware_interface };
    auto allocator = transient.create_linear_upload_allocator(1024, "small uploads");
    REQUIRE(allocator.has_value());

    CHECK(allocator->allocate(1024).has_value());
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const auto failed = allocator->allocate(1);
        REQUIRE_FALSE(failed.has_value());
        CHECK(rhi::is_error(failed.error(), rhi::error_kind::transient_memory_exhausted));
    }
    CHECK(count_lines(hardware_interface, "'small uploads' is exhausted") == 1);

    allocator->reset({});
    CHECK(allocator->allocate(1024).has_value());
    CHECK_FALSE(allocator->allocate(1).has_value());
    CHECK(count_lines(hardware_interface, "'small uploads' is exhausted") == 2);
}

TEST_CASE("linear_upload_allocator: write and reset", "[rhi]")
{
    struct constants
    {
        std::array<float, 4> color;
        std::uint32_t index;
    };

    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    rhi::transient_objects transient{ *hardware_interface };
    auto allocator = transient.create_linear_upload_allocator(64 * 1024);
    REQUIRE(allocator.has_value());

    const auto first = allocator->allocate(1); // offset 0: its addresses locate the mapping
    REQUIRE(first.has_value());
    const constants value{ .color = { 0.25f, 0.5f, 0.75f, 1.0f }, .index = 42 };
    const auto written = allocator->write(value);
    REQUIRE(written.has_value());
    CHECK(written->value % 256 == 0);
    const std::byte* const bytes = first->cpu.data() + (written->value - first->gpu.value);
    CHECK(std::memcmp(bytes, &value, sizeof(value)) == 0);

    SECTION("with complete points it rewinds to 0")
    {
        const rhi::synchronization_point used = tests::bare_signal(hardware_interface, rhi::queue_type::graphics);
        hardware_interface.wait_for(used);
        allocator->reset(std::array{ used });
        const auto again = allocator->allocate(1);
        REQUIRE(again.has_value());
        CHECK(again->gpu == first->gpu);
        CHECK(assertions.count() == 0);
    }

    SECTION("with a point that isn't complete it asserts")
    {
        hardware_interface.accept_errors();
        const rhi::synchronization_point used = gate.point(rhi::queue_type::graphics, 1);
        allocator->reset(std::array{ used });
#ifndef NDEBUG
        CHECK(assertions.contains("reset while a use isn't complete"));
#endif
    }
}

TEST_CASE("descriptor_range: views of placed textures, exhaustion and reset", "[rhi]")
{
    tests::test_rhi hardware_interface;
    rhi::transient_objects transient{ *hardware_interface };
    rhi::memory memory{ *hardware_interface };
    rhi::resources resources{ *hardware_interface };

    const auto heap = memory.create_heap({ .size = 1 << 20 });
    REQUIRE(heap.has_value());
    const auto texture = memory.create_placed_texture(
        *heap,
        0,
        { .width = 64,
          .height = 64,
          .format = rhi::format::rgba8_unorm,
          .usage = rhi::texture_usage::shader_resource | rhi::texture_usage::unordered_access });
    REQUIRE(texture.has_value());

    auto range = transient.create_descriptor_range(4, "views");
    REQUIRE(range.has_value());
    CHECK(range->capacity() == 4);

    const auto srv = range->create_srv(*texture);
    const auto uav = range->create_uav(*texture);
    REQUIRE(srv.has_value());
    REQUIRE(uav.has_value());
    CHECK(uav->value == srv->value + 1);
    // In the transient region, after the persistent one
    CHECK(srv->value > hardware_interface->state().descriptors().configuration().persistent_descriptor_count);
    CHECK(range->create_srv(*texture, { .first_mip = 0, .mip_count = 1 }).has_value());
    CHECK(range->create_uav(*texture).has_value());

    const auto full = range->create_srv(*texture);
    REQUIRE_FALSE(full.has_value());
    CHECK(rhi::is_error(full.error(), rhi::error_kind::transient_memory_exhausted));

    range->reset({});
    const auto reused = range->create_srv(*texture);
    REQUIRE(reused.has_value());
    CHECK(reused->value == srv->value);

    const auto stale = range->create_srv(rhi::texture_handle{});
    REQUIRE_FALSE(stale.has_value());
    CHECK(rhi::is_error(stale.error(), rhi::error_kind::invalid_handle));

    resources.destroy(*texture);
    memory.destroy(*heap);
}

TEST_CASE("Dropping a transient object while it is in use defers its state", "[rhi]")
{
    tests::test_rhi hardware_interface{ small_transient_region() };
    tests::rhi_gate gate{ hardware_interface };
    rhi::transient_objects transient{ *hardware_interface };

    {
        auto in_use = transient.create_descriptor_range(4); // 5 of the 8 transient descriptors
        REQUIRE(in_use.has_value());
        const auto point = gate.point(rhi::queue_type::graphics, 1); // the "use", not complete yet
        static_cast<void>(point);
    }
    // Still held: the range waits for the submission in flight when it was dropped
    rhi::completion_driver{ *hardware_interface }.poll_completions();
    const auto blocked = transient.create_descriptor_range(4);
    REQUIRE_FALSE(blocked.has_value());
    CHECK(rhi::is_error(blocked.error(), rhi::error_kind::descriptor_heap_full));

    gate.open(1);
    hardware_interface->wait_idle(); // completes the point and polls
    CHECK(transient.create_descriptor_range(4).has_value());
}

TEST_CASE("timestamp_query_set: creation", "[rhi]")
{
    tests::test_rhi hardware_interface;
    rhi::transient_objects transient{ *hardware_interface };

    auto set = transient.create_timestamp_query_set(rhi::queue_type::graphics, 16, "timings");
    REQUIRE(set.has_value());
    CHECK(set->queue() == rhi::queue_type::graphics);
    CHECK(set->capacity() == 16);
    CHECK(set->ticks(0, 16).size() == 16);

    const auto empty = transient.create_timestamp_query_set(rhi::queue_type::compute, 0);
    REQUIRE_FALSE(empty.has_value());
    CHECK(rhi::is_error(empty.error(), rhi::error_kind::invalid_argument));

    const auto copy = transient.create_timestamp_query_set(rhi::queue_type::copy, 4);
    CHECK(copy.has_value() == hardware_interface->capabilities().copy_queue_timestamps);
    if (!copy)
    {
        CHECK(rhi::is_error(copy.error(), rhi::error_kind::unsupported_hardware));
    }
}
