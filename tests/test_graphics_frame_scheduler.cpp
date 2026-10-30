// Frame scheduler: pacing, the frame token and the per-frame services (docs/tasks/07-graphics-frames-and-transfers.md
// §7.2, §7.3). Needs a D3D12 device (WARP is enough). The tests drive completions themselves.

#include "helpers/graphics_fixture.h"
#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"
#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"

#include "avis/error.h"
#include "avis/graphics/error.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/error.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/transient.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace avis;
using rhi::queue_type;

namespace
{
    std::size_t count_lines(const tests::log_capture& capture, const std::string& text)
    {
        const std::vector<std::string> lines = capture.lines();
        return static_cast<std::size_t>(
            std::ranges::count_if(lines, [&text](const std::string& line) { return line.contains(text); }));
    }

    constexpr const char* abandoned_message = "ended by its token without end_frame";

    // Could complete with set_value(), but always completes with set_stopped(): the "other branch" of a when_all that
    // stops after begin_frame delivered its token (sync_wait needs a value completion to accept the when_all)
    struct stopping_sender
    {
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>;

        template<class Receiver>
        struct operation
        {
            using operation_state_concept = stdexec::operation_state_tag;

            void start() & noexcept
            {
                stdexec::set_stopped(std::move(receiver));
            }

            Receiver receiver;
        };

        template<stdexec::receiver Receiver>
        operation<Receiver> connect(Receiver receiver) const
        {
            return { std::move(receiver) };
        }
    };

    std::vector<rhi::synchronization_point> points_of(const graphics::frame_token& frame)
    {
        const auto points = frame.recycled_submissions();
        return { points.begin(), points.end() };
    }
} // namespace

static_assert(avis::sender_of<graphics::frame_sender, graphics::frame_token>);

TEST_CASE("frame_scheduler: create checks its configuration", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const rhi::synchronization synchronization{ *hardware_interface };
    const rhi::transient_objects transient{ *hardware_interface };

    const auto created = [&](const graphics::frame_scheduler_configuration& configuration)
    { return graphics::frame_scheduler::create(synchronization, transient, configuration); };

    for (const std::uint32_t frames : { 1u, 2u, 3u })
    {
        const auto scheduler = created(tests::test_frame_scheduler_configuration(frames));
        REQUIRE(scheduler.has_value());
        CHECK((*scheduler)->frames_in_flight() == frames);
    }

    for (const std::uint32_t frames : { 0u, 4u })
    {
        const auto failed = created(tests::test_frame_scheduler_configuration(frames));
        REQUIRE_FALSE(failed.has_value());
        CHECK(graphics::is_error(failed.error(), graphics::error_kind::invalid_argument));
    }

    auto configuration = tests::test_frame_scheduler_configuration();
    configuration.upload_memory_per_frame = 0;
    CHECK(graphics::is_error(created(configuration).error(), graphics::error_kind::invalid_argument));
    configuration = tests::test_frame_scheduler_configuration();
    configuration.descriptors_per_frame = 0;
    CHECK(graphics::is_error(created(configuration).error(), graphics::error_kind::invalid_argument));
}

TEST_CASE(
    "frame_scheduler: descriptor ranges that don't fit the transient region pass the RHI's error through", "[graphics]")
{
    rhi::render_hardware_interface_configuration few_descriptors;
    few_descriptors.descriptor_heaps.transient_descriptor_count = 100;
    tests::test_rhi hardware_interface{ few_descriptors };

    auto configuration = tests::test_frame_scheduler_configuration(2);
    configuration.descriptors_per_frame = 60; // 2 × 60 > 100
    const auto failed = graphics::frame_scheduler::create(
        rhi::synchronization{ *hardware_interface }, rhi::transient_objects{ *hardware_interface }, configuration);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().domain == error_domain::rhi);
}

TEST_CASE(
    "frame_scheduler: frame N + F waits for frame N's submissions and completes from poll_completions", "[graphics]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    graphics::frame_token first = tests::begin_frame_inline(*scheduler);
    CHECK(first.index() == 1);
    CHECK(first.context_index() == 1);
    CHECK(first.completed_index() == 0);
    CHECK(first.recycled_submissions().empty());
    const rhi::synchronization_point held = gate.point(queue_type::graphics, 1);
    const std::array first_points{ held };
    scheduler->end_frame(std::move(first), first_points);

    graphics::frame_token second = tests::begin_frame_inline(*scheduler);
    CHECK(second.index() == 2);
    CHECK(second.context_index() == 0);
    CHECK(second.recycled_submissions().empty());
    scheduler->end_frame(std::move(second), {});

    tests::started_frame third{ scheduler->begin_frame() };
    third.start();
    CHECK(third.outcome.total() == 0);
    driver.poll_completions();
    CHECK(third.outcome.total() == 0);

    gate.open(1);
    hardware_interface.wait_for(held); // waits on the fence, then polls on this thread
    REQUIRE(third.outcome.values == 1);
    CHECK(third.outcome.thread == std::this_thread::get_id());

    graphics::frame_token& token = *third.outcome.token;
    CHECK(token.index() == 3);
    CHECK(token.context_index() == 1);
    CHECK(token.completed_index() == 2); // frame 2 had no submissions, so it is complete too
    CHECK(points_of(token) == std::vector{ held });
    scheduler->end_frame(std::move(token), {});
}

TEST_CASE("frame_scheduler: indices, completed index and recycled submissions over many frames", "[graphics]")
{
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 3);

    std::vector<std::vector<rhi::synchronization_point>> recorded{ {} }; // by frame index; frame 0 unused
    for (std::uint64_t index = 1; index <= 9; ++index)
    {
        graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
        CHECK(frame.index() == index);
        CHECK(frame.context_index() == index % 3);
        // Every earlier frame is complete: the newest one is reported, which is at least N − F
        CHECK(frame.completed_index() == index - 1);

        if (index <= 3)
        {
            CHECK(frame.recycled_submissions().empty());
        }
        else
        {
            CHECK(points_of(frame) == recorded[index - 3]);
        }

        // Two graphics points (only the latest is kept) and, on odd frames, a compute point
        const rhi::synchronization_point early = tests::submit_marker<queue_type::graphics>(hardware_interface);
        const rhi::synchronization_point late = tests::submit_marker<queue_type::graphics>(hardware_interface);
        std::vector<rhi::synchronization_point> submitted{ late, early };
        std::vector<rhi::synchronization_point> expected{ late };
        if (index % 2 == 1)
        {
            const rhi::synchronization_point compute = tests::submit_marker<queue_type::compute>(hardware_interface);
            submitted.push_back(compute);
            expected.push_back(compute);
        }
        recorded.push_back(expected);

        hardware_interface.wait_for(late);
        for (const rhi::synchronization_point point : expected)
        {
            hardware_interface.wait_for(point);
        }
        scheduler->end_frame(std::move(frame), submitted);
    }
}

TEST_CASE("frame_scheduler: try_begin_frame never waits", "[graphics]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    auto first = scheduler->try_begin_frame();
    REQUIRE(first.has_value());
    const std::array held{ gate.point(queue_type::compute, 1) };
    scheduler->end_frame(std::move(*first), held);

    auto second = scheduler->try_begin_frame();
    REQUIRE(second.has_value());
    CHECK(second->index() == 2);
    scheduler->end_frame(std::move(*second), {});

    CHECK_FALSE(scheduler->try_begin_frame().has_value());
    CHECK_FALSE(scheduler->try_begin_frame().has_value());

    gate.open(1);
    hardware_interface.wait_for(held[0]);
    auto third = scheduler->try_begin_frame();
    REQUIRE(third.has_value());
    CHECK(third->index() == 3);
    CHECK(points_of(*third) == std::vector{ held[0] });
    scheduler->end_frame(std::move(*third), {});

    // begin_frame and try_begin_frame share the pacing
    graphics::frame_token fourth = tests::begin_frame_inline(*scheduler);
    CHECK(fourth.index() == 4);
}

TEST_CASE("frame_scheduler: stopped and abandoned frames", "[graphics]")
{
    tests::log_capture graphics_log{ "graphics" };
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 1);

    SECTION("end_frame with no submissions frees the context at once, also with an unsubmitted list")
    {
        {
            graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
            auto list = rhi::command_lists{ *hardware_interface }.acquire<queue_type::graphics>("never submitted");
            REQUIRE(list.has_value());
            scheduler->end_frame(std::move(frame), {});
        } // the RHI discards the unsubmitted list

        graphics::frame_token next = tests::begin_frame_inline(*scheduler);
        CHECK(next.index() == 2);
        scheduler->end_frame(std::move(next), {});
        CHECK(count_lines(graphics_log, abandoned_message) == 0);
    }

    SECTION("dropping a token ends its frame")
    {
        {
            graphics::frame_token dropped = tests::begin_frame_inline(*scheduler);
            CHECK(dropped.index() == 1);
        }
        CHECK(count_lines(graphics_log, "frame 1 ended by its token without end_frame") == 1);
        graphics::frame_token next = tests::begin_frame_inline(*scheduler);
        CHECK(next.index() == 2);
    }

    SECTION("move-assigning over a token ends its frame; a moved-from token ends nothing")
    {
        graphics::frame_token moved_from = tests::begin_frame_inline(*scheduler);
        graphics::frame_token holder = std::move(moved_from);
        CHECK(holder.index() == 1);
        CHECK(moved_from.recycled_submissions().empty());

        holder = std::move(moved_from); // holder's frame 1 ends; it then holds nothing
        CHECK(count_lines(graphics_log, "frame 1 ended by its token") == 1);

        graphics::frame_token next = tests::begin_frame_inline(*scheduler);
        CHECK(next.index() == 2);
        scheduler->end_frame(std::move(next), {});
        // Destroying holder and moved_from (both empty) ends nothing more
    }

    SECTION("a when_all stopped by another branch drops the token it stored")
    {
        tests::recorded_assertions assertions;
        const auto outcome = stdexec::sync_wait(stdexec::when_all(scheduler->begin_frame(), stopping_sender{}));
        CHECK_FALSE(outcome.has_value());
        CHECK(count_lines(graphics_log, "frame 1 ended by its token") == 1);
        CHECK(assertions.count() == 0);

        graphics::frame_token next = tests::begin_frame_inline(*scheduler);
        CHECK(next.index() == 2);
        scheduler->end_frame(std::move(next), {});
    }
}

TEST_CASE("frame_scheduler: a stop request while waiting starts no frame", "[graphics]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    const rhi::synchronization_point held = gate.point(queue_type::graphics, 1);
    {
        graphics::frame_token first = tests::begin_frame_inline(*scheduler);
        const std::array points{ held };
        scheduler->end_frame(std::move(first), points);
    }
    scheduler->end_frame(tests::begin_frame_inline(*scheduler), {});

    {
        tests::started_frame third{ scheduler->begin_frame() };
        third.start();
        CHECK(third.outcome.total() == 0);
        third.source.request_stop();
        CHECK(third.outcome.stops == 1);
        CHECK(third.outcome.thread == std::this_thread::get_id());
    }

    // A later poll completes nothing more, and the next begin_frame is frame 3 again
    gate.open(1);
    hardware_interface.wait_for(held);
    graphics::frame_token third = tests::begin_frame_inline(*scheduler);
    CHECK(third.index() == 3);
    CHECK(points_of(third) == std::vector{ held });

    SECTION("a stop requested before start completes inline")
    {
        scheduler->end_frame(std::move(third), {});
        tests::started_frame stopped{ scheduler->begin_frame() };
        stopped.source.request_stop();
        stopped.start();
        CHECK(stopped.outcome.stops == 1);
        CHECK(tests::begin_frame_inline(*scheduler).index() == 4);
    }
}

TEST_CASE("frame_scheduler: device removal while a frame waits completes it with device_lost", "[graphics][removal]")
{
    tests::test_rhi hardware_interface;
    hardware_interface.accept_errors(); // the removal report and the debug layer's messages are errors
    tests::rhi_gate gate{ hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    {
        graphics::frame_token first = tests::begin_frame_inline(*scheduler);
        const std::array points{ gate.point(queue_type::graphics, 1) };
        scheduler->end_frame(std::move(first), points);
    }
    scheduler->end_frame(tests::begin_frame_inline(*scheduler), {});

    tests::started_frame third{ scheduler->begin_frame() };
    third.start();
    CHECK(third.outcome.total() == 0);

    hardware_interface->state().device().native().RemoveDevice();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 10 };
    while (third.outcome.total() == 0)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        driver.poll_completions();
        std::this_thread::yield();
    }
    REQUIRE(third.outcome.errors == 1);
    CHECK(rhi::is_error(*third.outcome.error, rhi::error_kind::device_lost));
    // No frame is outstanding: the scheduler's destructor doesn't assert
}

TEST_CASE("frame_scheduler: debug checks on pacing", "[graphics]")
{
    tests::recorded_assertions assertions;
    tests::log_capture graphics_log{ "graphics" };
    tests::test_rhi hardware_interface;
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);

    SECTION("a second begin_frame while one is outstanding")
    {
        graphics::frame_token first = tests::begin_frame_inline(*scheduler);

        tests::started_frame second{ scheduler->begin_frame() };
        second.start();
        REQUIRE(second.outcome.errors == 1);
        CHECK(second.outcome.error->code == static_cast<std::int32_t>(generic_error_kind::logic_error));
        CHECK_FALSE(scheduler->try_begin_frame().has_value());
#ifndef NDEBUG
        CHECK(assertions.contains("begin_frame while a frame is outstanding"));
#endif

        // The outstanding frame is unaffected
        scheduler->end_frame(std::move(first), {});
        assertions.clear();
        CHECK(tests::begin_frame_inline(*scheduler).index() == 2);
        CHECK(assertions.count() == 0);
    }

    SECTION("end_frame with a token of another scheduler")
    {
        const auto other = tests::make_test_frame_scheduler(hardware_interface, 2);
        graphics::frame_token foreign = tests::begin_frame_inline(*other);

        other.get()->end_frame(std::move(foreign), {}); // fine
        graphics::frame_token mine = tests::begin_frame_inline(*scheduler);
        graphics::frame_token theirs = tests::begin_frame_inline(*other);
        scheduler->end_frame(std::move(theirs), {});
#ifndef NDEBUG
        CHECK(assertions.contains("isn't a frame of this scheduler"));
#endif
        // Refused: theirs is still outstanding on the other scheduler, and ends with its destructor
        CHECK(theirs.index() == 2);
        CHECK_FALSE(other->try_begin_frame().has_value());
        scheduler->end_frame(std::move(mine), {});
    }
}
