#include "avis/io/asio_scheduler.h"
#include "avis/io/asio_use_sender.h"
#include "avis/io/io_context.h"
#include "avis/threading/thread_pool.h"

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("basic asio::io_context scheduler for senders & receivers", "[avis::io::scheduler]")
{
    asio::io_context ctx;
    avis::io::scheduler sched{ ctx };

    auto work1 = stdexec::just() | stdexec::then([] {});
    exec::start_detached(stdexec::starts_on(sched, std::move(work1)));

    ctx.run();
}

TEST_CASE("basic asio use_sender completion token for senders & receivers", "[avis::io::use_sender]")
{
    asio::io_context ctx;
    avis::io::scheduler sched{ ctx };

    asio::steady_timer timer{ ctx, std::chrono::seconds(3) };

    stdexec::sender auto work = timer.async_wait(avis::io::use_sender) | stdexec::then(
                                                                             [](asio::error_code ec)
                                                                             {
                                                                                 if (ec)
                                                                                 {
                                                                                     throw asio::system_error{ ec };
                                                                                 }
                                                                             });
    exec::start_detached(stdexec::starts_on(sched, std::move(work)));

    ctx.run();
}

TEST_CASE("use_sender on the pool-hosted io_context, stopped through the body's stop token", "[avis::io::use_sender]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context io{ pool, 1 };

    asio::steady_timer timer{ io.native(), std::chrono::milliseconds(10) };
    auto result = stdexec::sync_wait(timer.async_wait(avis::io::use_sender));
    REQUIRE(result.has_value());
    CHECK_FALSE(std::get<0>(*result));

    // The work guard is still held, so only the stop token can end the body
    pool.request_stop();
    pool.join();
    CHECK(pool.running() == 0);
}
