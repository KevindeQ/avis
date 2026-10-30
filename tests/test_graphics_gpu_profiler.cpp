// GPU profiler (docs/tasks/07-graphics-frames-and-transfers.md §7.4). Needs a D3D12 device (WARP is enough). Each test
// runs a frame scheduler and waits for its own submissions, so frame N − F is complete when frame N is prepared.

#include "helpers/graphics_fixture.h"
#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"
#include "helpers/rhi_fixture.h"

#include "avis/graphics/error.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/gpu_profiler.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/transient.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace avis;
using rhi::queue_type;

namespace
{
    template<queue_type Queue>
    rhi::command_list<Queue> acquire(tests::test_rhi& hardware_interface)
    {
        auto list = rhi::command_lists{ *hardware_interface }.acquire<Queue>("profiled");
        REQUIRE(list.has_value());
        return std::move(*list);
    }

    // Submits, waits for the GPU and returns the point
    template<queue_type Queue>
    rhi::synchronization_point submit(tests::test_rhi& hardware_interface, rhi::command_list<Queue>& list)
    {
        auto point = rhi::queue<Queue>{ *hardware_interface }.submit(std::span{ &list, 1 });
        REQUIRE(point.has_value());
        hardware_interface.wait_for(*point);
        return *point;
    }

    // A frame with one graphics scope named `name`, or none when `name` is empty
    void run_frame(
        tests::test_rhi& hardware_interface,
        graphics::frame_scheduler& scheduler,
        graphics::gpu_profiler* const profiler,
        const std::string& name)
    {
        graphics::frame_token frame = tests::begin_frame_inline(scheduler);
        if (profiler == nullptr || name.empty())
        {
            scheduler.end_frame(std::move(frame), {});
            return;
        }
        auto list = acquire<queue_type::graphics>(hardware_interface);
        const graphics::gpu_scope_id scope = profiler->begin_timing(frame, list, name);
        profiler->end_timing(frame, list, scope);
        const std::array points{ submit(hardware_interface, list) };
        scheduler.end_frame(std::move(frame), points);
    }

    std::vector<std::string> names_of(const std::span<const graphics::gpu_timing> timings)
    {
        std::vector<std::string> names;
        for (const graphics::gpu_timing& timing : timings)
        {
            names.emplace_back(timing.name);
        }
        return names;
    }

    std::size_t count_lines(const tests::log_capture& capture, const std::string& text)
    {
        const std::vector<std::string> lines = capture.lines();
        return static_cast<std::size_t>(
            std::ranges::count_if(lines, [&text](const std::string& line) { return line.contains(text); }));
    }
} // namespace

TEST_CASE("gpu_profiler: create checks its arguments", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto create = [&](const std::uint32_t frames, const std::uint32_t timestamps)
    {
        return graphics::gpu_profiler::create(
            rhi::transient_objects{ *hardware_interface },
            rhi::queue<queue_type::graphics>{ *hardware_interface },
            rhi::queue<queue_type::compute>{ *hardware_interface },
            rhi::queue<queue_type::copy>{ *hardware_interface },
            frames,
            { .timestamps_per_frame = timestamps });
    };

    CHECK(create(1, 2).has_value());
    CHECK(create(3, 1024).has_value());
    CHECK(graphics::is_error(create(0, 64).error(), graphics::error_kind::invalid_argument));
    CHECK(graphics::is_error(create(4, 64).error(), graphics::error_kind::invalid_argument));
    CHECK(graphics::is_error(create(2, 1).error(), graphics::error_kind::invalid_argument));
}

TEST_CASE("gpu_profiler: nested scopes on the graphics and compute queues", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);
    const auto profiler = tests::make_test_gpu_profiler(hardware_interface, 2);

    {
        graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
        profiler->prepare_frame(frame);
        CHECK(profiler->completed_gpu_timings(frame).empty());

        auto graphics_list = acquire<queue_type::graphics>(hardware_interface);
        const auto outer = profiler->begin_timing(frame, graphics_list, "outer");
        const auto inner = profiler->begin_timing(frame, graphics_list, "inner");
        CHECK(outer.is_valid());
        CHECK(inner.is_valid());
        profiler->end_timing(frame, graphics_list, inner);
        const auto sibling = profiler->begin_timing(frame, graphics_list, "sibling");
        profiler->end_timing(frame, graphics_list, sibling);
        profiler->end_timing(frame, graphics_list, outer);

        auto compute_list = acquire<queue_type::compute>(hardware_interface);
        const auto compute = profiler->begin_timing(frame, compute_list, "compute pass");
        profiler->end_timing(frame, compute_list, compute);

        const std::array points{ submit(hardware_interface, graphics_list), submit(hardware_interface, compute_list) };
        scheduler->end_frame(std::move(frame), points);
    }
    run_frame(hardware_interface, *scheduler, profiler.get(), {}); // frame 2: unprepared, times nothing

    graphics::frame_token third = tests::begin_frame_inline(*scheduler);
    profiler->prepare_frame(third);
    const std::span<const graphics::gpu_timing> timings = profiler->completed_gpu_timings(third);
    REQUIRE(timings.size() == 4);
    CHECK(names_of(timings) == std::vector<std::string>{ "outer", "inner", "sibling", "compute pass" });

    const graphics::gpu_timing& outer = timings[0];
    const graphics::gpu_timing& inner = timings[1];
    CHECK(outer.queue == queue_type::graphics);
    CHECK(inner.queue == queue_type::graphics);
    CHECK(timings[3].queue == queue_type::compute);
    CHECK(outer.depth == 0);
    CHECK(inner.depth == 1);
    CHECK(timings[2].depth == 1);
    CHECK(timings[3].depth == 0);
    for (const graphics::gpu_timing& timing : timings)
    {
        CHECK(timing.start_milliseconds >= 0.0);
        CHECK(timing.duration_milliseconds >= 0.0);
    }
    CHECK(outer.start_milliseconds == 0.0); // the first timestamp on the queue
    CHECK(timings[3].start_milliseconds == 0.0);
    CHECK(inner.start_milliseconds >= outer.start_milliseconds);
    CHECK(
        inner.start_milliseconds + inner.duration_milliseconds <=
        outer.start_milliseconds + outer.duration_milliseconds);
    scheduler->end_frame(std::move(third), {});
}

TEST_CASE("gpu_profiler: results are frame N - F's, with names that outlive the next frame's recording", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);
    const auto profiler = tests::make_test_gpu_profiler(hardware_interface, 2);

    for (std::uint64_t index = 1; index <= 6; ++index)
    {
        graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
        profiler->prepare_frame(frame);
        const std::span<const graphics::gpu_timing> completed = profiler->completed_gpu_timings(frame);
        if (index <= 2)
        {
            CHECK(completed.empty());
        }
        else
        {
            REQUIRE(completed.size() == 1);
            CHECK(completed[0].name == std::format("scope of frame {}", index - 2));
        }

        // The name is copied: the string is gone before the results are read
        auto list = acquire<queue_type::graphics>(hardware_interface);
        graphics::gpu_scope_id scope;
        {
            const std::string name = std::format("scope of frame {}", index);
            scope = profiler->begin_timing(frame, list, name);
        }
        profiler->end_timing(frame, list, scope);

        // Recording frame N doesn't touch frame N − F's names
        if (index > 2)
        {
            REQUIRE(completed.size() == 1);
            CHECK(completed[0].name == std::format("scope of frame {}", index - 2));
        }

        const std::array points{ submit(hardware_interface, list) };
        scheduler->end_frame(std::move(frame), points);
    }
}

TEST_CASE("gpu_profiler: a skipped prepare_frame leaves that frame's successor without results", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);
    const auto profiler = tests::make_test_gpu_profiler(hardware_interface, 2);

    std::vector<std::vector<std::string>> results(9); // by frame index
    for (std::uint64_t index = 1; index <= 8; ++index)
    {
        graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
        if (index == 3)
        {
            scheduler->end_frame(std::move(frame), {}); // stopped before prepare_frame: times nothing
            continue;
        }
        profiler->prepare_frame(frame);
        results[index] = names_of(profiler->completed_gpu_timings(frame));

        auto list = acquire<queue_type::graphics>(hardware_interface);
        const auto scope = profiler->begin_timing(frame, list, std::format("frame {}", index));
        profiler->end_timing(frame, list, scope);
        const std::array points{ submit(hardware_interface, list) };
        scheduler->end_frame(std::move(frame), points);
    }

    CHECK(results[4] == std::vector<std::string>{ "frame 2" });
    CHECK(results[5].empty()); // frame 3 wasn't prepared; frame 1's ticks aren't relabelled
    CHECK(results[6] == std::vector<std::string>{ "frame 4" });
    CHECK(results[7] == std::vector<std::string>{ "frame 5" });
    CHECK(results[8] == std::vector<std::string>{ "frame 6" });
}

TEST_CASE("gpu_profiler: running out of timestamps drops scopes with one warning", "[graphics]")
{
    tests::log_capture graphics_log{ "graphics" };
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 1);
    const auto profiler = tests::make_test_gpu_profiler(hardware_interface, 1, 8); // 4 scopes per queue

    {
        graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
        profiler->prepare_frame(frame);
        auto list = acquire<queue_type::graphics>(hardware_interface);
        std::vector<graphics::gpu_scope_id> scopes;
        for (int index = 0; index < 6; ++index)
        {
            const auto scope = profiler->begin_timing(frame, list, std::format("scope {}", index));
            CHECK(scope.is_valid() == (index < 4));
            profiler->end_timing(frame, list, scope); // ignores the dropped ones
        }
        const std::array points{ submit(hardware_interface, list) };
        scheduler->end_frame(std::move(frame), points);
    }
    CHECK(count_lines(graphics_log, "ran out of timestamps on the graphics queue (4 scopes)") == 1);

    graphics::frame_token next = tests::begin_frame_inline(*scheduler);
    profiler->prepare_frame(next);
    CHECK(
        names_of(profiler->completed_gpu_timings(next)) ==
        std::vector<std::string>{ "scope 0", "scope 1", "scope 2", "scope 3" });
    scheduler->end_frame(std::move(next), {});
}

TEST_CASE("gpu_profiler: a narrowed recorder times on its list's queue", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 1);
    const auto profiler = tests::make_test_gpu_profiler(hardware_interface, 1);

    {
        graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
        profiler->prepare_frame(frame);
        auto list = acquire<queue_type::graphics>(hardware_interface);
        rhi::compute_recorder narrowed = list.narrow<queue_type::compute>();
        const auto scope = profiler->begin_timing(frame, narrowed, "narrowed");
        profiler->end_timing(frame, narrowed, scope);
        const std::array points{ submit(hardware_interface, list) };
        scheduler->end_frame(std::move(frame), points);
    }

    graphics::frame_token next = tests::begin_frame_inline(*scheduler);
    profiler->prepare_frame(next);
    const auto timings = profiler->completed_gpu_timings(next);
    REQUIRE(timings.size() == 1);
    CHECK(timings[0].name == "narrowed");
    CHECK(timings[0].queue == queue_type::graphics);
    scheduler->end_frame(std::move(next), {});
}

TEST_CASE("gpu_profiler: copy-queue scopes follow the device's copy-queue timestamps", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 1);
    const auto profiler = tests::make_test_gpu_profiler(hardware_interface, 1);
    const bool copy_timestamps = hardware_interface->capabilities().copy_queue_timestamps &&
                                 rhi::queue<queue_type::copy>{ *hardware_interface }.timestamp_frequency() != 0;

    {
        graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
        profiler->prepare_frame(frame);
        auto copy_list = acquire<queue_type::copy>(hardware_interface);
        const auto copy_scope = profiler->begin_timing(frame, copy_list, "copy");
        CHECK(copy_scope.is_valid() == copy_timestamps);
        profiler->end_timing(frame, copy_list, copy_scope);
        auto graphics_list = acquire<queue_type::graphics>(hardware_interface);
        const auto graphics_scope = profiler->begin_timing(frame, graphics_list, "graphics");
        profiler->end_timing(frame, graphics_list, graphics_scope);
        const std::array points{ submit(hardware_interface, copy_list), submit(hardware_interface, graphics_list) };
        scheduler->end_frame(std::move(frame), points);
    }

    graphics::frame_token next = tests::begin_frame_inline(*scheduler);
    profiler->prepare_frame(next);
    const auto timings = profiler->completed_gpu_timings(next);
    if (copy_timestamps)
    {
        REQUIRE(timings.size() == 2);
        CHECK(timings[0].name == "graphics");
        CHECK(timings[1].name == "copy");
        CHECK(timings[1].queue == queue_type::copy);
    }
    else
    {
        REQUIRE(timings.size() == 1);
        CHECK(timings[0].name == "graphics");
    }
    scheduler->end_frame(std::move(next), {});
}

TEST_CASE("gpu_profiler: debug checks", "[graphics]")
{
    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);
    const auto profiler = tests::make_test_gpu_profiler(hardware_interface, 2);

    SECTION("prepare_frame twice for one frame")
    {
        graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
        profiler->prepare_frame(frame);
        CHECK(assertions.count() == 0);
        profiler->prepare_frame(frame);
#ifndef NDEBUG
        CHECK(assertions.contains("each token is prepared once"));
#endif
        scheduler->end_frame(std::move(frame), {});
    }

    SECTION("begin_timing for a frame that wasn't prepared")
    {
        graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
        auto list = acquire<queue_type::graphics>(hardware_interface);
        const auto scope = profiler->begin_timing(frame, list, "unprepared");
        CHECK_FALSE(scope.is_valid());
#ifndef NDEBUG
        CHECK(assertions.contains("begin_timing for a frame that wasn't prepared"));
#endif
        profiler->end_timing(frame, list, scope); // ignored
        scheduler->end_frame(std::move(frame), {});
    }
}
