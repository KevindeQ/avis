// Device removal (docs/tasks/06-rhi.md §6.12, docs/design/09-rhi.md §20.4). The tests remove the device with
// ID3D12Device5::RemoveDevice. D3D12 keeps one device per adapter in a process, so each test releases every reference
// to it before the next test creates a new one.

#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"

#include "avis/rhi/command_list.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/error.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/synchronization.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <latch>
#include <string>
#include <thread>
#include <vector>

using namespace avis;
using avis::tests::rhi_gate;
using avis::tests::started_wait;
using avis::tests::started_waits;
using rhi::queue_type;

namespace
{
    void remove_device(tests::test_rhi& hardware_interface)
    {
        hardware_interface->state().device().native().RemoveDevice();
    }

    // How many times the removal report was logged
    std::size_t removal_reports(const tests::test_rhi& hardware_interface)
    {
        const std::vector<std::string> lines = hardware_interface.log().lines();
        return static_cast<std::size_t>(std::ranges::count_if(
            lines, [](const std::string& line) { return line.contains("the D3D12 device was removed during"); }));
    }
} // namespace

TEST_CASE("After device removal, waits fail and synchronous calls throw device_lost", "[rhi][removal]")
{
    started_waits waits;
    tests::test_rhi hardware_interface;
    hardware_interface.accept_errors(); // the removal report and the debug layer's messages are errors
    rhi_gate gate{ hardware_interface };
    rhi::synchronization synchronization{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    const rhi::d3d12::device& device = hardware_interface->state().device();

    // Ten waits that the gate keeps from completing, on all three queues
    std::array<const started_wait*, 10> pending{};
    for (std::size_t index = 0; index < pending.size(); ++index)
    {
        const auto queue = static_cast<queue_type>(index % 3);
        pending[index] = &waits.start(synchronization.when_complete(gate.point(queue, index + 1)));
    }
    CHECK(driver.has_pending_work());

    // Acquired before the removal; acquiring one afterwards fails
    auto list = rhi::command_lists{ *hardware_interface }.acquire<queue_type::graphics>("before removal");
    REQUIRE(list.has_value());

    remove_device(hardware_interface);

    // The fences report the removal (UINT64_MAX) once D3D12 has processed it
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 10 };
    while (!device.lost())
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        driver.poll_completions();
        std::this_thread::yield();
    }
    driver.poll_completions();

    for (const started_wait* const wait : pending)
    {
        CHECK(wait->counts.errors == 1);
        CHECK(wait->counts.device_lost_errors == 1);
        CHECK(wait->counts.values == 0);
    }
    CHECK_FALSE(hardware_interface->state().completions().has_waiters());

    rhi::queue<queue_type::graphics> graphics{ *hardware_interface };
    CHECK_THROWS_AS(graphics.submit(std::span{ &*list, 1 }), rhi::device_lost);
    CHECK_THROWS_AS(hardware_interface->wait_idle(), rhi::device_lost);
    CHECK_THROWS_AS(hardware_interface->state().queues()[queue_type::copy].execute({}), rhi::device_lost);

    const auto acquired = rhi::command_lists{ *hardware_interface }.acquire<queue_type::compute>();
    REQUIRE_FALSE(acquired.has_value());
    CHECK(rhi::is_error(acquired.error(), rhi::error_kind::device_lost));

    // A new wait fails inline, on the calling thread
    const started_wait& late = waits.start(
        synchronization.when_complete(rhi::synchronization_point{ .queue = queue_type::graphics, .value = 1 }));
    CHECK(late.counts.device_lost_errors == 1);
    CHECK(late.counts.thread == std::this_thread::get_id());

    CHECK(removal_reports(hardware_interface) == 1);
    // The gate's and the RHI's destructors run next and must not hang
}

TEST_CASE("The removal report is logged once when four threads see the removal at once", "[rhi][removal]")
{
    tests::test_rhi hardware_interface;
    hardware_interface.accept_errors();
    rhi::completion_driver driver{ *hardware_interface };
    rhi::d3d12::device_state& state = hardware_interface->state();

    // Work in flight on every queue, so each thread has a fence to find removed
    for (const queue_type queue : { queue_type::graphics, queue_type::compute, queue_type::copy })
    {
        tests::bare_signal(hardware_interface, queue);
    }

    remove_device(hardware_interface);

    std::latch start{ 4 };
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread)
    {
        threads.emplace_back(
            [&, thread]
            {
                start.arrive_and_wait();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 10 };
                while (!state.device().lost() && std::chrono::steady_clock::now() < deadline)
                {
                    switch (thread)
                    {
                    case 0:
                        driver.poll_completions();
                        break;
                    case 1:
                        try
                        {
                            hardware_interface->wait_idle();
                        }
                        catch (...)
                        {
                            // device_lost once the removal is seen; checked through lost() below
                        }
                        break;
                    default:
                        static_cast<void>(state.queues()[static_cast<queue_type>(thread - 2)].completed());
                        break;
                    }
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    REQUIRE(state.device().lost());
    CHECK(removal_reports(hardware_interface) == 1);
}
