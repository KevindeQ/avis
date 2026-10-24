// Tests for the pool-hosted IO context

#include "helpers/log_capture.h"
#include "helpers/thread_count.h"

#include "avis/io/asio_use_sender.h"
#include "avis/io/detail/registered_operation.h"
#include "avis/io/io_context.h"
#include "avis/log.h"
#include "avis/threading/parallel_helpers.h"
#include "avis/threading/thread_pool.h"

#include <asio/steady_timer.hpp>
#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

using namespace std::chrono_literals;

TEST_CASE("schedule completes on a pool thread named avis-io-0", "[io_context]")
{
    avis::threading::thread_pool pool{ { .capacity = 2 } };
    avis::io::io_context io{ pool, { .threads = 1 } };

    auto result = stdexec::sync_wait(
        stdexec::schedule(io.get_scheduler()) | stdexec::then(
                                                    []
                                                    {
                                                        return std::tuple{
                                                            avis::threading::thread_pool::current_index(),
                                                            avis::io::io_context::current(),
                                                            std::string{ avis::log::thread_name() },
                                                        };
                                                    }));
    REQUIRE(result.has_value());
    const auto [index, context, name] = std::get<0>(*result);
    CHECK(index.has_value());
    CHECK(context == &io);
    CHECK(name == "avis-io-0");
    CHECK(avis::io::io_context::current() == nullptr);
}

TEST_CASE("a timer completes through the pool-hosted context", "[io_context]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, { .threads = 1 } };

    asio::steady_timer timer{ io.native(), 10ms };
    CHECK(stdexec::sync_wait(timer.async_wait(avis::io::use_sender)).has_value());
}

TEST_CASE("destroying the io_context stops its bodies", "[io_context]")
{
    avis::threading::thread_pool pool{ { .capacity = 2 } };
    {
        avis::io::io_context io{ pool, { .threads = 2 } };
        CHECK(pool.running() == 2);
    }
    CHECK(pool.running() == 0);
}

TEST_CASE("the pool's stop request alone stops the IO bodies", "[io_context]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, { .threads = 1 } };
    REQUIRE(pool.running() == 1);

    // Backstop path: the work guard is still held, only the stop token tells the body to return
    pool.request_stop();
    pool.join();
    CHECK(pool.running() == 0);
}

TEST_CASE("sync_wait refuses to block an IO thread", "[io_context]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, { .threads = 1 } };

    auto nested = stdexec::schedule(io.get_scheduler()) |
                  stdexec::then([] { return avis::threading::sync_wait(stdexec::just(1)).has_value(); });
    CHECK_THROWS_AS(stdexec::sync_wait(std::move(nested)), std::logic_error);
}

TEST_CASE("sync_wait_result refuses to block an IO thread", "[io_context][sync_wait_result]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, { .threads = 1 } };

    auto nested = stdexec::schedule(io.get_scheduler()) |
                  stdexec::then([] { return avis::threading::sync_wait_result(stdexec::just(1)).has_value(); });
    CHECK_THROWS_AS(stdexec::sync_wait(std::move(nested)), std::logic_error);

    // From this thread it waits for IO senders, which already complete with avis::error
    asio::steady_timer timer{ io.native(), 10ms };
    CHECK(avis::threading::sync_wait_result(timer.async_wait(avis::io::use_sender)).has_value());
}

TEST_CASE("an io_context that can't start its bodies throws and leaves no thread", "[io_context]")
{
    const std::size_t before = avis::tests::current_process_thread_count();
    avis::threading::thread_pool pool{ { .capacity = 1 } };

    CHECK_THROWS_AS((avis::io::io_context{ pool, { .threads = 2 } }), avis::runtime_error); // only 1 slot
    CHECK_THROWS_AS((avis::io::io_context{ pool, { .threads = 0 } }), avis::runtime_error);
    CHECK(pool.running() == 0);
    CHECK(avis::tests::current_process_thread_count() == before);
}

namespace
{
    // A registered operation that ignores cancellation, standing in for an IO operation that never completes
    class stuck_operation final : public avis::io::detail::registered_operation
    {
    public:
        void cancel() noexcept override
        {
            ++cancel_calls;
        }

        std::atomic<int> cancel_calls = 0;
    };

    // Waits until flag is set, at most timeout; true when it was set
    bool wait_for_flag(const std::atomic<bool>& flag, const std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!flag.load())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }
} // namespace

TEST_CASE("request_stop cancels a running operation and returns once it completed", "[io_context][shutdown]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, { .threads = 1, .shutdown_timeout = 5s } };

    // start_detached's receiver can't be stopped: only the registry can cancel this wait
    asio::steady_timer timer{ io.native(), 10s };
    std::atomic<bool> stopped = false;
    exec::start_detached(
        timer.async_wait(avis::io::use_sender) | stdexec::then([]() noexcept {}) |
        stdexec::upon_stopped([&stopped]() noexcept { stopped.store(true); }) |
        stdexec::upon_error([](avis::error) noexcept {}));

    CHECK_FALSE(io.stopping());
    const auto begin = std::chrono::steady_clock::now();
    io.request_stop();
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    CHECK(io.stopping());
    CHECK(elapsed < 1s);
    CHECK(wait_for_flag(stopped, 1s));
}

TEST_CASE("request_stop gives up on an operation that ignores cancellation", "[io_context][shutdown]")
{
    SECTION("after the shutdown timeout, with a warning")
    {
        avis::threading::thread_pool pool{ { .capacity = 1 } };
        avis::io::io_context io{ pool, { .threads = 1, .shutdown_timeout = 100ms } };
        avis::tests::log_capture capture{ "io" };

        avis::io::detail::operation_registry* const registry = avis::io::detail::find_operation_registry(io.native());
        REQUIRE(registry != nullptr);
        stuck_operation operation;
        REQUIRE(avis::io::detail::add_operation(*registry, operation));

        const auto begin = std::chrono::steady_clock::now();
        io.request_stop();
        const auto elapsed = std::chrono::steady_clock::now() - begin;

        CHECK(operation.cancel_calls == 1);
        CHECK(elapsed >= 90ms);
        CHECK(elapsed < 2s);
        CHECK(capture.contains("[warning] io_context: 1 operation(s) still running after the 100 ms shutdown timeout"));

        avis::io::detail::remove_operation(*registry, operation);
    }

    SECTION("immediately with a shutdown timeout of 0")
    {
        avis::threading::thread_pool pool{ { .capacity = 1 } };
        avis::io::io_context io{ pool, { .threads = 1, .shutdown_timeout = 0ms } };
        avis::tests::log_capture capture{ "io" };

        avis::io::detail::operation_registry* const registry = avis::io::detail::find_operation_registry(io.native());
        REQUIRE(registry != nullptr);
        stuck_operation operation;
        REQUIRE(avis::io::detail::add_operation(*registry, operation));

        const auto begin = std::chrono::steady_clock::now();
        io.request_stop();
        const auto elapsed = std::chrono::steady_clock::now() - begin;

        CHECK(operation.cancel_calls == 1);
        CHECK(elapsed < 50ms);
        CHECK_FALSE(capture.contains("shutdown timeout"));

        avis::io::detail::remove_operation(*registry, operation);
    }
}

TEST_CASE("after request_stop, new operations complete with set_stopped without starting", "[io_context][shutdown]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, { .threads = 1 } };
    io.request_stop();

    avis::io::detail::operation_registry* const registry = avis::io::detail::find_operation_registry(io.native());
    REQUIRE(registry != nullptr);
    stuck_operation operation;
    CHECK_FALSE(avis::io::detail::add_operation(*registry, operation));

    // Completes inline in start(): the context no longer runs anything
    asio::steady_timer timer{ io.native(), 10s };
    CHECK_FALSE(stdexec::sync_wait(timer.async_wait(avis::io::use_sender)).has_value());
}

TEST_CASE("a plain asio::io_context has no operation registry", "[io_context][shutdown]")
{
    asio::io_context context;
    CHECK(avis::io::detail::find_operation_registry(context) == nullptr);
}
