// Tests for the pool-hosted IO context

#include "helpers/thread_count.h"

#include "avis/io/asio_use_sender.h"
#include "avis/io/io_context.h"
#include "avis/log.h"
#include "avis/threading/parallel_helpers.h"
#include "avis/threading/thread_pool.h"

#include <asio/steady_timer.hpp>
#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

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
    avis::io::io_context io{ pool, 1 };

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
    avis::io::io_context io{ pool, 1 };

    asio::steady_timer timer{ io.native(), 10ms };
    auto result = stdexec::sync_wait(timer.async_wait(avis::io::use_sender));
    REQUIRE(result.has_value());
    CHECK_FALSE(std::get<0>(*result));
}

TEST_CASE("destroying the io_context stops its bodies", "[io_context]")
{
    avis::threading::thread_pool pool{ { .capacity = 2 } };
    {
        avis::io::io_context io{ pool, 2 };
        CHECK(pool.running() == 2);
    }
    CHECK(pool.running() == 0);
}

TEST_CASE("the pool's stop request alone stops the IO bodies", "[io_context]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, 1 };
    REQUIRE(pool.running() == 1);

    // Backstop path: the work guard is still held, only the stop token tells the body to return
    pool.request_stop();
    pool.join();
    CHECK(pool.running() == 0);
}

TEST_CASE("sync_wait refuses to block an IO thread", "[io_context]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, 1 };

    auto nested = stdexec::schedule(io.get_scheduler()) |
                  stdexec::then([] { return avis::threading::sync_wait(stdexec::just(1)).has_value(); });
    CHECK_THROWS_AS(stdexec::sync_wait(std::move(nested)), std::logic_error);
}

TEST_CASE("an io_context that can't start its bodies throws and leaves no thread", "[io_context]")
{
    const std::size_t before = avis::tests::current_process_thread_count();
    avis::threading::thread_pool pool{ { .capacity = 1 } };

    CHECK_THROWS_AS((avis::io::io_context{ pool, 2 }), avis::runtime_error); // only 1 slot
    CHECK_THROWS_AS((avis::io::io_context{ pool, 0 }), avis::runtime_error);
    CHECK(pool.running() == 0);
    CHECK(avis::tests::current_process_thread_count() == before);
}
