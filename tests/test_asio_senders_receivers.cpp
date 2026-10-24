#include "avis/io/asio_scheduler.h"
#include "avis/io/asio_use_sender.h"
#include "avis/io/io_context.h"
#include "avis/sender_of.h"
#include "avis/threading/thread_pool.h"

#include <asio/io_context.hpp>
#include <asio/random_access_file.hpp>
#include <asio/steady_timer.hpp>
#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("basic asio::io_context scheduler for senders & receivers", "[avis::io::scheduler]")
{
    asio::io_context context;
    avis::io::scheduler scheduler{ context };

    bool ran = false;
    auto work = stdexec::just() | stdexec::then([&ran] { ran = true; });
    exec::start_detached(stdexec::starts_on(scheduler, std::move(work)));

    context.run();
    CHECK(ran);
}

TEST_CASE("basic asio use_sender completion token for senders & receivers", "[avis::io::use_sender]")
{
    asio::io_context context;
    avis::io::scheduler scheduler{ context };

    asio::steady_timer timer{ context, 10ms };

    bool completed = false;
    stdexec::sender auto work =
        timer.async_wait(avis::io::use_sender) | stdexec::then([&completed] { completed = true; });
    exec::start_detached(stdexec::starts_on(scheduler, std::move(work)));

    context.run();
    CHECK(completed);
}

TEST_CASE("use_sender senders have the Avis completion signatures", "[avis::io::use_sender]")
{
    asio::io_context context;
    asio::steady_timer timer{ context };
    asio::random_access_file file{ context };
    std::array<std::byte, 16> buffer{};
    avis::io::scheduler scheduler{ context };

    // The leading error code is peeled off the asio signature
    STATIC_CHECK(avis::sender_of<decltype(timer.async_wait(avis::io::use_sender))>);
    STATIC_CHECK(
        avis::sender_of<decltype(file.async_read_some_at(0, asio::buffer(buffer), avis::io::use_sender)), std::size_t>);
    STATIC_CHECK(avis::sender_of<decltype(stdexec::schedule(scheduler))>);
}

TEST_CASE("use_sender names the operation after the asio initiation", "[avis::io::use_sender]")
{
    using avis::io::detail::operation_name_from;

    CHECK(
        operation_name_from(
            "class asio::basic_waitable_timer<struct std::chrono::steady_clock>::initiate_async_wait") == "async_wait");
    CHECK(
        operation_name_from("class asio::basic_random_access_file<>::initiate_async_read_some_at") ==
        "async_read_some_at");
    CHECK(operation_name_from("struct some_lambda") == "asio operation");
    CHECK(operation_name_from("struct x::initiate_") == "asio operation");
}

TEST_CASE("use_sender completes on the IO thread, even when the receiver has a scheduler", "[avis::io::use_sender]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, { .threads = 1 } };

    // sync_wait's environment carries its run_loop scheduler; the sender doesn't hop to it
    asio::steady_timer timer{ io.native(), 10ms };
    auto result = stdexec::sync_wait(
        timer.async_wait(avis::io::use_sender) | stdexec::then([] { return avis::io::io_context::current(); }));

    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == &io);
}

TEST_CASE("use_sender completes with set_stopped when its stop token fires", "[avis::io::use_sender]")
{
    avis::threading::thread_pool pool{ { .capacity = 2 } };
    avis::io::io_context io{ pool, { .threads = 2 } };

    constexpr int iterations = 1000;
    int not_stopped = 0;
    int too_slow = 0;

    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        asio::steady_timer timer{ io.native(), 10s };
        stdexec::inplace_stop_source source;

        // Stop before, during or after start(), from another thread
        std::thread stopper{ [&source, iteration]
                             {
                                 for (int spin = 0; spin < (iteration % 8) * 50; ++spin)
                                 {
                                     std::atomic_signal_fence(std::memory_order_seq_cst);
                                 }
                                 source.request_stop();
                             } };

        const auto begin = std::chrono::steady_clock::now();
        auto result = stdexec::sync_wait(
            stdexec::write_env(
                timer.async_wait(avis::io::use_sender), stdexec::prop{ stdexec::get_stop_token, source.get_token() }));
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        stopper.join();

        if (result.has_value())
        {
            ++not_stopped;
        }
        if (elapsed > 5s)
        {
            ++too_slow;
        }
    }

    CHECK(not_stopped == 0);
    CHECK(too_slow == 0);
}

TEST_CASE("use_sender cancels an operation that is already running in asio", "[avis::io::use_sender]")
{
    avis::threading::thread_pool pool{ { .capacity = 2 } };
    avis::io::io_context io{ pool, { .threads = 2 } };

    constexpr int iterations = 100;
    int not_stopped = 0;

    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        asio::steady_timer timer{ io.native(), 10s };
        asio::steady_timer trigger{ io.native(), std::chrono::microseconds{ 100 + (iteration % 10) * 100 } };
        stdexec::inplace_stop_source source;
        std::atomic<bool> trigger_done = false;

        // The stop request comes from an IO thread, after the 10 s wait was handed to asio
        exec::start_detached(
            trigger.async_wait(avis::io::use_sender) |
            stdexec::then(
                [&source, &trigger_done]() noexcept
                {
                    source.request_stop();
                    trigger_done.store(true);
                    trigger_done.notify_one();
                }) |
            stdexec::upon_error(
                [&trigger_done](avis::error) noexcept
                {
                    trigger_done.store(true);
                    trigger_done.notify_one();
                }));

        auto result = stdexec::sync_wait(
            stdexec::write_env(
                timer.async_wait(avis::io::use_sender), stdexec::prop{ stdexec::get_stop_token, source.get_token() }));
        if (result.has_value())
        {
            ++not_stopped;
        }

        // request_stop() may still be returning on the IO thread; source and trigger must outlive it
        trigger_done.wait(false);
    }

    CHECK(not_stopped == 0);
}

TEST_CASE("a failing asio operation completes with set_error(avis::error)", "[avis::io::use_sender]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, { .threads = 1 } };

    asio::random_access_file file{ io.native() }; // never opened
    std::array<std::byte, 16> buffer{};
    std::optional<avis::error> captured;

    auto work = file.async_read_some_at(0, asio::buffer(buffer), avis::io::use_sender) |
                stdexec::then([](std::size_t) noexcept {}) |
                stdexec::upon_error([&captured](avis::error error) noexcept { captured = std::move(error); });
    REQUIRE(stdexec::sync_wait(std::move(work)).has_value());

    REQUIRE(captured.has_value());
    CHECK(captured->domain == avis::error_domain::io);
    CHECK(captured->message.starts_with("async_read_some_at: "));
}

TEST_CASE("use_sender on the pool-hosted io_context, stopped through the body's stop token", "[avis::io::use_sender]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, { .threads = 1 } };

    asio::steady_timer timer{ io.native(), 10ms };
    CHECK(stdexec::sync_wait(timer.async_wait(avis::io::use_sender)).has_value());

    // The work guard is still held, so only the stop token can end the body
    pool.request_stop();
    pool.join();
    CHECK(pool.running() == 0);
}
