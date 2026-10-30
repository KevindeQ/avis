// gpu_completion_thread (docs/tasks/07-graphics-frames-and-transfers.md §7.8). Needs a D3D12 device (WARP is enough).
// Unlike the other graphics tests, nothing here calls poll_completions(): the thread drives the RHI.

#include "helpers/graphics_fixture.h"
#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"

#include "avis/error.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/gpu_completion_thread.h"
#include "avis/log.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/synchronization.h"
#include "avis/threading/thread_pool.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

using namespace avis;
using rhi::queue_type;

namespace
{
    using namespace std::chrono_literals;

    // The pool prefixes the names of its threads in logs and debuggers
    const std::string completion_thread_name = "avis-" + std::string{ graphics::gpu_completion_thread::thread_name };

    // Where a completion ran, published to the test thread
    struct completion_record
    {
        std::atomic<bool> done{ false };
        int values = 0;
        int others = 0;
        std::string thread;

        void record(const bool value) noexcept
        {
            (value ? values : others) += 1;
            try
            {
                thread = std::string{ log::thread_name() };
            }
            catch (...)
            {}
            done.store(true, std::memory_order_release);
        }

        // Waits for the completion without driving the RHI
        bool wait() const
        {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!done.load(std::memory_order_acquire))
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    return false;
                }
                std::this_thread::yield();
            }
            return true;
        }
    };

    struct gpu_wait_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value() noexcept
        {
            record->record(true);
        }

        void set_error(avis::error) noexcept
        {
            record->record(false);
        }

        void set_stopped() noexcept
        {
            record->record(false);
        }

        completion_record* record;
    };

    struct frame_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value(graphics::frame_token frame) noexcept
        {
            token->emplace(std::move(frame));
            record->record(true);
        }

        void set_error(avis::error) noexcept
        {
            record->record(false);
        }

        void set_stopped() noexcept
        {
            record->record(false);
        }

        completion_record* record;
        std::optional<graphics::frame_token>* token;
    };

    // A started begin_frame whose token arrives on whichever thread completes it
    class driven_frame
    {
    public:
        explicit driven_frame(graphics::frame_sender sender) :
            operation_{ stdexec::connect(std::move(sender), frame_receiver{ &record, &token }) }
        {
            stdexec::start(operation_);
        }

        driven_frame(const driven_frame&) = delete;
        driven_frame& operator=(const driven_frame&) = delete;

        completion_record record;
        std::optional<graphics::frame_token> token;

    private:
        stdexec::connect_result_t<graphics::frame_sender, frame_receiver> operation_;
    };

    std::unique_ptr<graphics::gpu_completion_thread>
        start_completion_thread(threading::thread_pool& pool, tests::test_rhi& hardware_interface)
    {
        auto thread = graphics::gpu_completion_thread::create(pool, rhi::completion_driver{ *hardware_interface });
        REQUIRE(thread.has_value());
        return std::move(*thread);
    }
} // namespace

TEST_CASE("gpu_completion_thread: a GPU wait completes on the thread once its point is reached", "[graphics]")
{
    tests::test_rhi hardware_interface;
    threading::thread_pool pool{ { .capacity = 2 } };
    tests::rhi_gate gate{ hardware_interface };
    rhi::synchronization synchronization{ *hardware_interface };
    const auto thread = start_completion_thread(pool, hardware_interface);
    CHECK(pool.running() == 1);

    const rhi::synchronization_point point = gate.point(queue_type::graphics, 1);
    completion_record record;
    auto operation = stdexec::connect(synchronization.when_complete(point), gpu_wait_receiver{ &record });
    stdexec::start(operation);
    std::this_thread::sleep_for(20ms);
    CHECK_FALSE(record.done.load());

    gate.open(1);
    REQUIRE(record.wait());
    CHECK(record.values == 1);
    CHECK(record.thread == completion_thread_name);
}

TEST_CASE("gpu_completion_thread: drives 100 frames of a frame scheduler", "[graphics]")
{
    constexpr std::uint64_t frames = 100;

    tests::test_rhi hardware_interface;
    threading::thread_pool pool{ { .capacity = 2 } };
    tests::rhi_gate gate{ hardware_interface };
    const auto scheduler = tests::make_test_frame_scheduler(hardware_interface, 2);
    const auto thread = start_completion_thread(pool, hardware_interface);

    // Each frame's submission is held by the gate until the frame two later waits for it, so every begin_frame
    // after the first two completes on the thread
    int inline_frames = 0;
    int driven_frames = 0;
    for (std::uint64_t index = 1; index <= frames; ++index)
    {
        driven_frame frame{ scheduler->begin_frame() };
        if (index > 2)
        {
            CHECK_FALSE(frame.record.done.load());
            gate.open(index - 2);
            REQUIRE(frame.record.wait());
            driven_frames += frame.record.thread == completion_thread_name ? 1 : 0;
        }
        else
        {
            REQUIRE(frame.record.done.load());
            ++inline_frames;
        }
        REQUIRE(frame.token.has_value());
        CHECK(frame.token->index() == index);

        static_cast<void>(gate.point(queue_type::graphics, index));
        const std::array submissions{ tests::submit_marker<queue_type::graphics>(hardware_interface) };
        scheduler->end_frame(std::move(*frame.token), submissions);
    }
    CHECK(inline_frames == 2);
    CHECK(driven_frames == static_cast<int>(frames - 2));
    gate.open(frames);
}

TEST_CASE("gpu_completion_thread: stops promptly and leaves nothing pending", "[graphics]")
{
    tests::test_rhi hardware_interface;
    threading::thread_pool pool{ { .capacity = 2 } };
    rhi::completion_driver driver{ *hardware_interface };

    SECTION("when destroyed")
    {
        auto thread = start_completion_thread(pool, hardware_interface);

        // Something to drive first, so the thread has been through a poll
        completion_record record;
        auto operation = stdexec::connect(
            rhi::synchronization{ *hardware_interface }.when_complete(
                tests::submit_marker<queue_type::copy>(hardware_interface)),
            gpu_wait_receiver{ &record });
        stdexec::start(operation);
        REQUIRE(record.wait());

        const auto start = std::chrono::steady_clock::now();
        thread.reset();
        CHECK(std::chrono::steady_clock::now() - start < 1s);
        CHECK(pool.running() == 0);
        CHECK_FALSE(driver.has_pending_work());
    }

    SECTION("when its pool stops first")
    {
        auto thread = start_completion_thread(pool, hardware_interface);
        pool.request_stop();
        pool.join(); // returns only once the body has seen the stop request
        CHECK(pool.running() == 0);
        thread.reset();
    }
}

TEST_CASE("gpu_completion_thread: creation fails when the pool is full", "[graphics]")
{
    tests::test_rhi hardware_interface;
    threading::thread_pool pool{ { .capacity = 1 } };
    const auto first = start_completion_thread(pool, hardware_interface);

    const auto second = graphics::gpu_completion_thread::create(pool, rhi::completion_driver{ *hardware_interface });
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().domain == error_domain::generic);
    CHECK(second.error().code == static_cast<std::int32_t>(generic_error_kind::pool_exhausted));
}
