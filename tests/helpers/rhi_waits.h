#ifndef AVIS_TESTS_HELPERS_RHI_WAITS_H
#define AVIS_TESTS_HELPERS_RHI_WAITS_H

// Helpers for tests of GPU waits and deferred releases (docs/tasks/06-rhi.md §6.4, §6.5): a gate that holds a queue
// back until the test opens it, and started gpu_wait_senders that count their completions.

#include "rhi_fixture.h"

#include "avis/error.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/native_access.h"
#include "avis/rhi/detail/gpu_wait_sender.h"
#include "avis/rhi/queue.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace avis::tests
{
    // A test fence that queues wait on. point() returns a point that is reached only once open() passed its value,
    // so a test controls exactly when the GPU gets there. The destructor opens everything and waits for the GPU, so
    // a test that stops early never leaves a queue blocked (declare the gate after the test_rhi).
    class rhi_gate
    {
    public:
        explicit rhi_gate(test_rhi& rhi) : rhi_{ rhi }
        {
            REQUIRE(
                SUCCEEDED(rhi->state().device().native().CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))));
        }

        ~rhi_gate()
        {
            fence_->Signal(fully_open);
            try
            {
                rhi_->wait_idle();
            }
            catch (...)
            {}
        }

        rhi_gate(const rhi_gate&) = delete;
        rhi_gate& operator=(const rhi_gate&) = delete;

        // `queue` waits for the gate to reach `value`, then signals its own timeline (an execute with no lists)
        rhi::synchronization_point point(const rhi::queue_type queue, const std::uint64_t value)
        {
            REQUIRE(SUCCEEDED(rhi::d3d12::native_queue(*rhi_, queue).Wait(fence_.get(), value)));
            auto signalled = rhi_->state().queues()[queue].execute({});
            REQUIRE(signalled.has_value());
            return { .queue = queue, .value = *signalled };
        }

        // CPU-side Signal
        void open(const std::uint64_t value)
        {
            REQUIRE(SUCCEEDED(fence_->Signal(value)));
        }

    private:
        static constexpr std::uint64_t fully_open = std::uint64_t{ 1 } << 62;

        test_rhi& rhi_;
        rhi::com_ptr<ID3D12Fence> fence_;
    };

    // An execute with no lists: the next point on `queue`'s timeline
    inline rhi::synchronization_point bare_signal(test_rhi& rhi, const rhi::queue_type queue)
    {
        auto signalled = rhi->state().queues()[queue].execute({});
        REQUIRE(signalled.has_value());
        return { .queue = queue, .value = *signalled };
    }

    struct wait_counts
    {
        std::atomic<int> values{ 0 };
        std::atomic<int> errors{ 0 };
        std::atomic<int> stops{ 0 };
        std::atomic<std::thread::id> thread{};

        int total() const noexcept
        {
            return values.load() + errors.load() + stops.load();
        }
    };

    struct wait_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value() noexcept
        {
            counts->thread = std::this_thread::get_id();
            ++counts->values;
            if (then)
            {
                then();
            }
        }

        void set_error(avis::error) noexcept
        {
            counts->thread = std::this_thread::get_id();
            ++counts->errors;
        }

        void set_stopped() noexcept
        {
            counts->thread = std::this_thread::get_id();
            ++counts->stops;
        }

        auto get_env() const noexcept
        {
            return stdexec::prop{ stdexec::get_stop_token, token };
        }

        wait_counts* counts;
        stdexec::inplace_stop_token token;
        std::function<void()> then; // runs inside set_value
    };

    // A connected gpu_wait_sender with its counts and stop source. Not movable: keep it in a unique_ptr.
    struct started_wait
    {
        started_wait(rhi::gpu_wait_sender sender, std::function<void()> then) :
            operation{ stdexec::connect(
                std::move(sender), wait_receiver{ &counts, source.get_token(), std::move(then) }) }
        {}

        wait_counts counts;
        stdexec::inplace_stop_source source;
        stdexec::connect_result_t<rhi::gpu_wait_sender, wait_receiver> operation;
    };

    // Owns the waits a test starts. Declare it before the test_rhi, so that a wait still queued when a test stops early
    // is completed with set_stopped by the RHI's destructor before its operation state goes away.
    class started_waits
    {
    public:
        started_wait& start(rhi::gpu_wait_sender sender, std::function<void()> then = {})
        {
            auto& wait = *waits_.emplace_back(std::make_unique<started_wait>(std::move(sender), std::move(then)));
            stdexec::start(wait.operation);
            return wait;
        }

        // Connects without starting
        started_wait& connect(rhi::gpu_wait_sender sender, std::function<void()> then = {})
        {
            return *waits_.emplace_back(std::make_unique<started_wait>(std::move(sender), std::move(then)));
        }

        const std::vector<std::unique_ptr<started_wait>>& all() const noexcept
        {
            return waits_;
        }

    private:
        std::vector<std::unique_ptr<started_wait>> waits_;
    };
} // namespace avis::tests

#endif
