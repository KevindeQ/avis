// Shutdown order and concurrent use of the RHI (docs/tasks/06-rhi.md §6.12, docs/design/09-rhi.md §19.3, §19.4).
// In Debug every RHI mutex is a checking leaf_mutex, so a nested RHI lock fails the stress test through
// AVIS_RHI_CHECK.

#include "helpers/log_capture.h"
#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"

#include "avis/rhi/command_list.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/command_list_pool.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace avis;
using avis::tests::started_wait;
using avis::tests::started_waits;
using rhi::buffer_usage;
using rhi::format;
using rhi::queue_type;
using rhi::texture_usage;

namespace
{
    constexpr std::uint64_t buffer_size = 64 * 1024;

    template<queue_type Queue>
    rhi::synchronization_point copy_and_submit(
        rhi::render_hardware_interface& hardware_interface,
        const rhi::buffer_handle target,
        const rhi::buffer_handle source,
        std::mutex* const submission_mutex = nullptr)
    {
        // Throws instead of REQUIRE: workers call this, and Catch2 assertions aren't thread-safe
        auto list = rhi::command_lists{ hardware_interface }.acquire<Queue>("stress");
        if (!list)
        {
            throw std::runtime_error{ "acquire failed: " + list.error().message };
        }
        list->copy_buffer(target, 0, source, 0, buffer_size);

        std::unique_lock<std::mutex> lock;
        if (submission_mutex != nullptr)
        {
            lock = std::unique_lock{ *submission_mutex };
        }
        auto point = rhi::queue<Queue>{ hardware_interface }.submit(std::span{ &*list, 1 });
        if (!point)
        {
            throw std::runtime_error{ "submit failed: " + point.error().message };
        }
        return *point;
    }

    rhi::synchronization_point copy_and_submit(
        rhi::render_hardware_interface& hardware_interface,
        const queue_type queue,
        const rhi::buffer_handle target,
        const rhi::buffer_handle source,
        std::mutex* const submission_mutex = nullptr)
    {
        switch (queue)
        {
        case queue_type::graphics:
            return copy_and_submit<queue_type::graphics>(hardware_interface, target, source, submission_mutex);
        case queue_type::compute:
            return copy_and_submit<queue_type::compute>(hardware_interface, target, source, submission_mutex);
        case queue_type::copy:
            break;
        }
        return copy_and_submit<queue_type::copy>(hardware_interface, target, source, submission_mutex);
    }

    struct pool_sizes
    {
        std::size_t allocators = 0;
        std::uint32_t textures = 0;
        std::uint32_t buffers = 0;
    };

    pool_sizes sizes_of(rhi::d3d12::device_state& state)
    {
        return { .allocators = state.command_lists().allocators_created(),
                 .textures = state.resources().texture_slots_used(),
                 .buffers = state.resources().buffer_slots_used() };
    }
} // namespace

TEST_CASE("Destroying the RHI after a burst of work frees everything and stops pending waits", "[rhi]")
{
    tests::log_capture capture{ "rhi" };
    started_waits waits; // outlives the RHI, whose destructor stops the pending wait
    std::optional<tests::test_rhi> hardware_interface;
    hardware_interface.emplace();
    rhi::render_hardware_interface& rhi_object = **hardware_interface;
    rhi::d3d12::device_state& state = rhi_object.state();
    rhi::resources resources{ rhi_object };

    // A burst of submissions and destroys, nothing waited for
    std::vector<rhi::buffer_handle> buffers;
    for (int index = 0; index < 32; ++index)
    {
        auto source = resources.create_buffer({ .size = buffer_size, .usage = buffer_usage::copy_source });
        auto target = resources.create_buffer({ .size = buffer_size, .usage = buffer_usage::copy_destination });
        auto texture = resources.create_texture(
            { .width = 64, .height = 64, .format = format::rgba8_unorm, .usage = texture_usage::shader_resource });
        REQUIRE(source.has_value());
        REQUIRE(target.has_value());
        REQUIRE(texture.has_value());
        copy_and_submit(rhi_object, static_cast<queue_type>(index % 3), *target, *source);
        resources.destroy(*source);
        resources.destroy(*texture);
        buffers.push_back(*target); // left alive for the RHI's destructor
    }
    for (std::size_t index = 0; index < buffers.size(); index += 2)
    {
        resources.destroy(buffers[index]);
    }

    // A wait on a value that is never submitted
    const rhi::synchronization_point never{ .queue = queue_type::graphics,
                                            .value = state.queues()[queue_type::graphics].last_submitted() + 100 };
    const started_wait& pending = waits.start(rhi::synchronization{ rhi_object }.when_complete(never));
    CHECK(pending.counts.total() == 0);

    // Kept past the RHI: the fence shows the destructor waited for the GPU, and the device's reference count shows
    // nothing of the RHI's is left (every child object holds a reference to its device)
    std::array<std::uint64_t, 3> submitted{};
    std::array<ID3D12Fence*, 3> fences{};
    for (const queue_type queue : { queue_type::graphics, queue_type::compute, queue_type::copy })
    {
        const auto index = static_cast<std::size_t>(queue);
        submitted[index] = state.queues()[queue].last_submitted();
        fences[index] = &state.queues()[queue].fence();
        fences[index]->AddRef();
    }
    ID3D12Device* const device = &state.device().native();
    device->AddRef();

    hardware_interface.reset();

    for (std::size_t index = 0; index < fences.size(); ++index)
    {
        CHECK(fences[index]->GetCompletedValue() >= submitted[index]);
        fences[index]->Release();
    }
    CHECK(device->Release() == 0);

    CHECK(pending.counts.stops == 1);
    CHECK(capture.contains("1 GPU wait(s) still pending when the RHI was destroyed"));
}

TEST_CASE("Eight workers and two pollers use the RHI concurrently for ten seconds", "[rhi][stress]")
{
    tests::test_rhi hardware_interface;
    rhi::render_hardware_interface& rhi_object = *hardware_interface;
    rhi::d3d12::device_state& state = rhi_object.state();
    rhi::completion_driver driver{ rhi_object };
    std::array<std::mutex, 3> submission_mutexes; // queue<Queue>::submit isn't thread-safe against itself

    constexpr int worker_count = 8;
    constexpr auto duration = std::chrono::seconds{ 10 };
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + duration;

    std::atomic<bool> stop_polling{ false };
    std::atomic<std::size_t> iterations{ 0 };
    std::atomic<std::size_t> failures{ 0 };
    std::exception_ptr first_failure;
    std::mutex failure_mutex;

    std::vector<std::thread> pollers;
    for (int index = 0; index < 2; ++index)
    {
        pollers.emplace_back(
            [&]
            {
                void* const event = driver.completion_event();
                while (!stop_polling.load(std::memory_order_relaxed))
                {
                    WaitForSingleObject(event, 1);
                    driver.poll_completions();
                }
            });
    }

    std::vector<std::thread> workers;
    for (int worker = 0; worker < worker_count; ++worker)
    {
        workers.emplace_back(
            [&, worker]
            {
                try
                {
                    rhi::resources resources{ rhi_object };
                    rhi::synchronization synchronization{ rhi_object };
                    for (std::size_t round = 0; std::chrono::steady_clock::now() < deadline; ++round)
                    {
                        auto source =
                            resources.create_buffer({ .size = buffer_size, .usage = buffer_usage::copy_source });
                        auto target =
                            resources.create_buffer({ .size = buffer_size, .usage = buffer_usage::copy_destination });
                        auto texture = resources.create_texture(
                            { .width = 32,
                              .height = 32,
                              .format = format::rgba8_unorm,
                              .usage = texture_usage::shader_resource,
                              .debug_name = "stress texture" });
                        if (!source || !target || !texture)
                        {
                            failures.fetch_add(1);
                            break;
                        }

                        const auto queue = static_cast<queue_type>((round + static_cast<std::size_t>(worker)) % 3);
                        const rhi::synchronization_point point = copy_and_submit(
                            rhi_object, queue, *target, *source, &submission_mutexes[static_cast<std::size_t>(queue)]);

                        // Destroyed while the copy may still run: the deferred release holds them
                        resources.destroy(*source);
                        resources.destroy(*target);
                        resources.destroy(*texture);

                        // Completed by the pollers
                        stdexec::sync_wait(synchronization.when_complete(point));
                        iterations.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                catch (...)
                {
                    failures.fetch_add(1);
                    const std::lock_guard lock{ failure_mutex };
                    if (!first_failure)
                    {
                        first_failure = std::current_exception();
                    }
                }
            });
    }

    // Halfway, the pools should have reached their working size
    std::this_thread::sleep_until(start + duration / 2);
    const pool_sizes halfway = sizes_of(state);

    for (std::thread& worker : workers)
    {
        worker.join();
    }
    stop_polling = true;
    for (std::thread& poller : pollers)
    {
        poller.join();
    }
    const pool_sizes end = sizes_of(state);

    if (first_failure)
    {
        std::rethrow_exception(first_failure);
    }
    CHECK(failures == 0);
    INFO(
        "iterations " << iterations.load() << ", allocators " << halfway.allocators << " -> " << end.allocators
                      << ", texture slots " << halfway.textures << " -> " << end.textures << ", buffer slots "
                      << halfway.buffers << " -> " << end.buffers);
    CHECK(iterations > 1000);

    // Steady state: each worker's lists reuse its allocators, and destroyed slots are reused at once
    CHECK(end.allocators <= halfway.allocators + 3);
    CHECK(end.textures <= static_cast<std::uint32_t>(worker_count) * 2);
    CHECK(end.buffers <= static_cast<std::uint32_t>(worker_count) * 4);

    rhi_object.wait_idle();
    CHECK(state.lifetime().pending() == 0);
    CHECK_FALSE(driver.has_pending_work());
}
