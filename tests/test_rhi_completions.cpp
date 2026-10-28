// Queues, completion tracking and GPU waits (docs/tasks/06-rhi.md §6.4). Needs a D3D12 device (WARP is enough).
// The tests drive completions themselves: no driver thread runs.

#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"

#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/synchronization.h"
#include "avis/threading/parallel_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace avis;
using avis::tests::rhi_gate;
using avis::tests::started_wait;
using avis::tests::started_waits;

namespace
{
    // Spins until the GPU reached the point, without completing anything (poll_completions isn't called)
    void wait_on_gpu(tests::test_rhi& hardware_interface, const rhi::synchronization_point point)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 10 };
        while (!hardware_interface->state().queues()[point.queue].is_complete(point.value))
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }

    bool event_signalled(void* const event, const DWORD milliseconds = 0)
    {
        return WaitForSingleObject(event, milliseconds) == WAIT_OBJECT_0;
    }
} // namespace

TEST_CASE("Queues report their timestamp frequency and last submitted point", "[rhi]")
{
    tests::test_rhi hardware_interface;
    const rhi::queue<rhi::queue_type::graphics> graphics{ *hardware_interface };
    const rhi::queue<rhi::queue_type::copy> copy{ *hardware_interface };

    CHECK(graphics.timestamp_frequency() == hardware_interface->capabilities().timestamp_frequency);
    CHECK(graphics.last_submitted() == rhi::synchronization_point{ rhi::queue_type::graphics, 0 });

    const rhi::synchronization_point first = tests::bare_signal(hardware_interface, rhi::queue_type::copy);
    const rhi::synchronization_point second = tests::bare_signal(hardware_interface, rhi::queue_type::copy);
    CHECK(first.value == 1);
    CHECK(second.value == 2);
    CHECK(copy.last_submitted() == second);
    hardware_interface.wait_for(second);
}

TEST_CASE("A wait on a reached point completes inline in start()", "[rhi]")
{
    started_waits waits;
    tests::test_rhi hardware_interface;
    rhi::synchronization synchronization{ *hardware_interface };

    SECTION("value 0")
    {
        const rhi::synchronization_point nothing{ rhi::queue_type::compute, 0 };
        CHECK(synchronization.is_complete(nothing));
        const started_wait& wait = waits.start(synchronization.when_complete(nothing));
        CHECK(wait.counts.values == 1);
        CHECK(wait.counts.thread == std::this_thread::get_id());
    }

    SECTION("a completed execute")
    {
        const rhi::synchronization_point point = tests::bare_signal(hardware_interface, rhi::queue_type::graphics);
        hardware_interface.wait_for(point);
        CHECK(synchronization.is_complete(point));
        const started_wait& wait = waits.start(synchronization.when_complete(point));
        CHECK(wait.counts.values == 1);
    }

    CHECK_FALSE(rhi::completion_driver{ *hardware_interface }.has_pending_work());
}

TEST_CASE("A wait on an unreached point completes from poll_completions once it is reached", "[rhi]")
{
    started_waits waits;
    tests::test_rhi hardware_interface;
    rhi_gate gate{ hardware_interface };
    rhi::synchronization synchronization{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };

    const rhi::synchronization_point point = gate.point(rhi::queue_type::graphics, 1);
    const rhi::synchronization_point later = gate.point(rhi::queue_type::graphics, 2);
    const started_wait& wait = waits.start(synchronization.when_complete(point));
    const started_wait& other = waits.start(synchronization.when_complete(later)); // stays pending

    CHECK(wait.counts.total() == 0);
    CHECK_FALSE(synchronization.is_complete(point));
    CHECK(driver.poll_completions() == 0);
    CHECK(wait.counts.total() == 0);

    gate.open(1);
    CHECK(hardware_interface.wait_for(point) == 1);
    CHECK(wait.counts.values == 1);
    CHECK(wait.counts.thread == std::this_thread::get_id());
    CHECK(synchronization.is_complete(point));
    CHECK(other.counts.total() == 0);

    gate.open(2);
    CHECK(hardware_interface.wait_for(later) == 1);
    CHECK(other.counts.values == 1);
}

TEST_CASE("A wait on all three queues completes after the last one is reached", "[rhi]")
{
    started_waits waits;
    tests::test_rhi hardware_interface;
    rhi_gate gate{ hardware_interface };
    rhi::synchronization synchronization{ *hardware_interface };

    const std::array points{ gate.point(rhi::queue_type::graphics, 1),
                             gate.point(rhi::queue_type::compute, 2),
                             gate.point(rhi::queue_type::copy, 3) };

    SECTION("reached in queue order")
    {
        const started_wait& wait = waits.start(synchronization.when_complete(points));
        gate.open(1);
        CHECK(hardware_interface.wait_for(points[0]) == 0);
        gate.open(2);
        CHECK(hardware_interface.wait_for(points[1]) == 0);
        CHECK(wait.counts.total() == 0);
        gate.open(3);
        CHECK(hardware_interface.wait_for(points[2]) == 1);
        CHECK(wait.counts.values == 1);
    }

    SECTION("all reached before one poll")
    {
        const started_wait& wait = waits.start(synchronization.when_complete(points));
        gate.open(3);
        for (const rhi::synchronization_point point : points)
        {
            wait_on_gpu(hardware_interface, point);
        }
        CHECK(rhi::completion_driver{ *hardware_interface }.poll_completions() == 1);
        CHECK(wait.counts.values == 1);
    }
}

TEST_CASE("The completion event is signalled when a waited-for value is reached", "[rhi]")
{
    started_waits waits;
    tests::test_rhi hardware_interface;
    rhi_gate gate{ hardware_interface };
    rhi::synchronization synchronization{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    void* const event = driver.completion_event();
    REQUIRE(event != nullptr);

    CHECK_FALSE(event_signalled(event));
    const rhi::synchronization_point point = gate.point(rhi::queue_type::compute, 1);
    const started_wait& wait = waits.start(synchronization.when_complete(point));
    CHECK_FALSE(event_signalled(event));

    gate.open(1);
    CHECK(event_signalled(event, 10'000));
    CHECK(driver.poll_completions() == 1);
    CHECK(wait.counts.values == 1);
    CHECK_FALSE(event_signalled(event)); // auto-reset, nothing due
}

TEST_CASE("A stop request removes a pending wait", "[rhi]")
{
    started_waits waits;
    tests::test_rhi hardware_interface;
    rhi_gate gate{ hardware_interface };
    rhi::synchronization synchronization{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };

    SECTION("before the point is reached")
    {
        const rhi::synchronization_point point = gate.point(rhi::queue_type::graphics, 1);
        started_wait& wait = waits.start(synchronization.when_complete(point));
        CHECK(driver.has_pending_work());

        wait.source.request_stop();
        CHECK(wait.counts.stops == 1);
        CHECK(wait.counts.thread == std::this_thread::get_id());
        CHECK_FALSE(driver.has_pending_work());

        gate.open(1);
        CHECK(hardware_interface.wait_for(point) == 0);
        CHECK(wait.counts.total() == 1);
    }

    SECTION("before start")
    {
        const rhi::synchronization_point point = gate.point(rhi::queue_type::graphics, 1);
        started_wait& wait = waits.connect(synchronization.when_complete(point));
        wait.source.request_stop();
        stdexec::start(wait.operation);
        CHECK(wait.counts.stops == 1);
        CHECK_FALSE(driver.has_pending_work());
    }

    SECTION("racing poll_completions on another thread, 1000 times")
    {
        int wrong = 0;
        for (std::uint64_t round = 1; round <= 1000; ++round)
        {
            const rhi::synchronization_point point = gate.point(rhi::queue_type::graphics, round);
            started_wait& wait = waits.start(synchronization.when_complete(point));
            gate.open(round);
            wait_on_gpu(hardware_interface, point); // due: poll_completions would complete it

            std::atomic<bool> go{ false };
            std::thread stopper{ [&]
                                 {
                                     while (!go.load())
                                     {
                                     }
                                     wait.source.request_stop();
                                 } };
            go = true;
            driver.poll_completions();
            stopper.join();
            driver.poll_completions();

            if (wait.counts.total() != 1)
            {
                ++wrong;
            }
        }
        CHECK(wrong == 0);
        CHECK_FALSE(driver.has_pending_work());
    }
}

TEST_CASE("Concurrent poll_completions calls complete every wait exactly once", "[rhi]")
{
    constexpr std::uint64_t point_count = 100;
    constexpr int waits_per_point = 100;
    constexpr int total_waits = static_cast<int>(point_count) * waits_per_point;

    started_waits waits;
    tests::test_rhi hardware_interface;
    rhi_gate gate{ hardware_interface };
    rhi::synchronization synchronization{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };

    std::vector<rhi::synchronization_point> points;
    for (std::uint64_t value = 1; value <= point_count; ++value)
    {
        const auto queue = static_cast<rhi::queue_type>(value % 3);
        points.push_back(gate.point(queue, value));
        for (int index = 0; index < waits_per_point; ++index)
        {
            waits.start(synchronization.when_complete(points.back()));
        }
    }

    std::atomic<int> completed{ 0 };
    std::vector<std::thread> drivers;
    for (int thread = 0; thread < 4; ++thread)
    {
        drivers.emplace_back(
            [&]
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 30 };
                while (completed.load() < total_waits && std::chrono::steady_clock::now() < deadline)
                {
                    completed += static_cast<int>(driver.poll_completions());
                }
            });
    }
    for (std::uint64_t value = 1; value <= point_count; ++value)
    {
        gate.open(value);
        std::this_thread::yield();
    }
    for (std::thread& thread : drivers)
    {
        thread.join();
    }
    // A request that came while another caller held the lock may be left to that caller's next round: drain it
    for (const rhi::synchronization_point point : points)
    {
        completed += static_cast<int>(hardware_interface.wait_for(point));
    }

    CHECK(completed == total_waits);
    int wrong = 0;
    for (const auto& wait : waits.all())
    {
        if (wait->counts.values != 1 || wait->counts.total() != 1)
        {
            ++wrong;
        }
    }
    CHECK(wrong == 0);
}

TEST_CASE("A completion may start a new wait without deadlocking", "[rhi]")
{
    started_waits waits;
    tests::test_rhi hardware_interface;
    rhi_gate gate{ hardware_interface };
    rhi::synchronization synchronization{ *hardware_interface };

    const rhi::synchronization_point first = gate.point(rhi::queue_type::graphics, 1);
    const rhi::synchronization_point second = gate.point(rhi::queue_type::copy, 2);
    started_wait* inner_pending = nullptr;
    started_wait* inner_reached = nullptr;
    const started_wait& outer = waits.start(
        synchronization.when_complete(first),
        [&]
        {
            inner_pending = &waits.start(synchronization.when_complete(second));
            inner_reached = &waits.start(synchronization.when_complete(first));
        });

    gate.open(1);
    CHECK(hardware_interface.wait_for(first) == 1);
    CHECK(outer.counts.values == 1);
    REQUIRE(inner_pending != nullptr);
    REQUIRE(inner_reached != nullptr);
    CHECK(inner_reached->counts.values == 1); // inline, inside the outer completion
    CHECK(inner_pending->counts.total() == 0);

    gate.open(2);
    CHECK(hardware_interface.wait_for(second) == 1);
    CHECK(inner_pending->counts.values == 1);
}

TEST_CASE("has_pending_work and wait_idle", "[rhi]")
{
    started_waits waits;
    tests::test_rhi hardware_interface;
    rhi_gate gate{ hardware_interface };
    rhi::synchronization synchronization{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };

    CHECK_FALSE(driver.has_pending_work());
    const rhi::synchronization_point point = gate.point(rhi::queue_type::copy, 1);
    const started_wait& wait = waits.start(synchronization.when_complete(point));
    CHECK(driver.has_pending_work());

    gate.open(1);
    hardware_interface->wait_idle(); // waits for every queue, then completes the wait
    CHECK(wait.counts.values == 1);
    CHECK_FALSE(driver.has_pending_work());

    SECTION("refused where blocking waits are forbidden")
    {
        const threading::detail::forbid_blocking_waits forbidden;
        CHECK_THROWS_AS(hardware_interface->wait_idle(), std::logic_error);
    }

    SECTION("refused on a parallel worker")
    {
        const threading::detail::parallel_work_scope worker;
        CHECK_THROWS_AS(hardware_interface->wait_idle(), std::logic_error);
    }
}

#ifndef NDEBUG
TEST_CASE("Debug builds warn when GPU waits pend without poll_completions", "[rhi]")
{
    started_waits waits;
    tests::test_rhi hardware_interface;
    rhi_gate gate{ hardware_interface };
    rhi::synchronization synchronization{ *hardware_interface };

    waits.start(synchronization.when_complete(gate.point(rhi::queue_type::graphics, 1)));
    std::this_thread::sleep_for(std::chrono::milliseconds{ 1100 });
    waits.start(synchronization.when_complete(gate.point(rhi::queue_type::graphics, 2))); // the check runs in add

    bool warned = false;
    for (const std::string& line : hardware_interface.log().lines())
    {
        warned = warned || (line.starts_with("[warning]") && line.contains("poll_completions() hasn't run"));
    }
    CHECK(warned);
}
#endif
