#ifndef AVIS_TESTS_HELPERS_GRAPHICS_FIXTURE_H
#define AVIS_TESTS_HELPERS_GRAPHICS_FIXTURE_H

// Helpers for the graphics-layer tests (docs/tasks/07-graphics-frames-and-transfers.md §7.1), on top of test_rhi:
// test-sized configurations for the graphics objects, an empty submission that yields a synchronization point, and
// begin_frame operations a test starts and inspects.

#include "rhi_fixture.h"

#include "avis/error.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/types.h"

#include <stdexec/execution.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace avis::tests
{
    // Test sizes: small enough to exhaust on purpose, large enough for the tests' normal use
    inline constexpr std::size_t test_staging_ring_size = std::size_t{ 1 } << 20; // 1 MiB
    inline constexpr std::size_t test_frame_memory_size = std::size_t{ 64 } << 10; // 64 KiB per frame context
    inline constexpr std::uint32_t test_frame_descriptor_count = 64; // per frame context

    // Acquires a list on `Queue`, records nothing, submits it and returns its point. Explicitly instantiated for the
    // three queue types in graphics_fixture.cpp.
    template<rhi::queue_type Queue>
    rhi::synchronization_point submit_marker(test_rhi& hardware_interface);

    // The configuration make_test_frame_scheduler uses: test_frame_memory_size and test_frame_descriptor_count
    graphics::frame_scheduler_configuration test_frame_scheduler_configuration(std::uint32_t frames_in_flight = 2);

    std::unique_ptr<graphics::frame_scheduler>
        make_test_frame_scheduler(test_rhi& hardware_interface, std::uint32_t frames_in_flight = 2);

    // What a begin_frame operation completed with
    struct frame_outcome
    {
        std::optional<graphics::frame_token> token;
        std::optional<avis::error> error;
        int values = 0;
        int errors = 0;
        int stops = 0;
        std::thread::id thread{};

        int total() const noexcept
        {
            return values + errors + stops;
        }
    };

    struct frame_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value(graphics::frame_token token) noexcept
        {
            outcome->thread = std::this_thread::get_id();
            outcome->token.emplace(std::move(token));
            ++outcome->values;
        }

        void set_error(avis::error error) noexcept
        {
            outcome->thread = std::this_thread::get_id();
            outcome->error.emplace(std::move(error));
            ++outcome->errors;
        }

        void set_stopped() noexcept
        {
            outcome->thread = std::this_thread::get_id();
            ++outcome->stops;
        }

        auto get_env() const noexcept
        {
            return stdexec::prop{ stdexec::get_stop_token, token };
        }

        frame_outcome* outcome;
        stdexec::inplace_stop_token token;
    };

    // A connected begin_frame with its outcome and stop source. Not movable. Declare it after the scheduler, so a
    // frame still waiting when a test stops early is stopped (and a delivered token ended) before the scheduler goes.
    class started_frame
    {
    public:
        explicit started_frame(graphics::frame_sender sender) :
            operation_{ stdexec::connect(std::move(sender), frame_receiver{ &outcome, source.get_token() }) }
        {}

        ~started_frame()
        {
            source.request_stop();
        }

        started_frame(const started_frame&) = delete;
        started_frame& operator=(const started_frame&) = delete;

        void start() noexcept
        {
            stdexec::start(operation_);
        }

        frame_outcome outcome;
        stdexec::inplace_stop_source source;

    private:
        stdexec::connect_result_t<graphics::frame_sender, frame_receiver> operation_;
    };

    // Starts begin_frame and requires it to complete inline with a token
    graphics::frame_token begin_frame_inline(graphics::frame_scheduler& scheduler);
} // namespace avis::tests

#endif
