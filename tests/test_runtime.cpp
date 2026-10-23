// Tests for avis::runtime

#include "helpers/log_capture.h"
#include "helpers/runtime_fixture.h"
#include "helpers/thread_count.h"

#include "avis/io/io_context.h"
#include "avis/log.h"
#include "avis/runtime.h"
#include "avis/threading/parallel_helpers.h"
#include "avis/threading/thread_pool.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <latch>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    // Blocks until stop is requested
    void wait_for_stop(const std::stop_token& token)
    {
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock{ mutex };
        condition.wait(lock, token, [] { return false; });
    }

    // A path whose parent directory can't be created, because its grandparent is a regular file
    std::filesystem::path uncreatable_log_path()
    {
        const auto blocker = std::filesystem::temp_directory_path() / "avis-runtime-test-blocker";
        std::ofstream{ blocker } << "not a directory";
        return blocker / "logs" / "avis.log";
    }
} // namespace

TEST_CASE("a runtime can be constructed, destroyed and constructed again", "[runtime]")
{
    const std::size_t before = avis::tests::current_process_thread_count();
    for (int round = 0; round < 3; ++round)
    {
        avis::runtime runtime{ avis::tests::small_runtime_configuration() };
        CHECK(avis::runtime::alive());
    }
    CHECK_FALSE(avis::runtime::alive());
    CHECK(avis::tests::current_process_thread_count() == before);
}

TEST_CASE("only one runtime may be alive at a time", "[runtime]")
{
    {
        avis::runtime first{ avis::tests::small_runtime_configuration() };
        CHECK_THROWS_AS(avis::runtime{ avis::tests::small_runtime_configuration() }, avis::runtime_error);
        CHECK(avis::runtime::alive()); // the failed second construction didn't touch the first
    }

    // Two threads racing: exactly one succeeds. The winner stays alive until the loser has tried.
    std::atomic<int> successes = 0;
    std::atomic<int> failures = 0;
    std::latch start{ 2 };
    std::latch attempted{ 2 };
    {
        std::vector<std::jthread> threads;
        for (int index = 0; index < 2; ++index)
        {
            threads.emplace_back(
                [&]
                {
                    start.arrive_and_wait();
                    try
                    {
                        avis::runtime runtime{ avis::tests::small_runtime_configuration() };
                        successes.fetch_add(1);
                        attempted.arrive_and_wait();
                    }
                    catch (const avis::runtime_error&)
                    {
                        failures.fetch_add(1);
                        attempted.arrive_and_wait();
                    }
                });
        }
    }
    CHECK(successes.load() == 1);
    CHECK(failures.load() == 1);
    CHECK_FALSE(avis::runtime::alive());
}

TEST_CASE("a failed construction rolls back and leaves no thread", "[runtime]")
{
    const std::size_t before = avis::tests::current_process_thread_count();

    SECTION("capacity below the threads needed")
    {
        auto configuration = avis::tests::small_runtime_configuration();
        configuration.thread_pool.capacity = 1; // io 1 + application 1 needs 2
        CHECK_THROWS_AS(avis::runtime{ configuration }, avis::runtime_error);
    }
    SECTION("no IO thread")
    {
        auto configuration = avis::tests::small_runtime_configuration();
        configuration.io.threads = 0;
        CHECK_THROWS_AS(avis::runtime{ configuration }, avis::runtime_error);
    }
    SECTION("a log file that can't be created")
    {
        auto configuration = avis::tests::small_runtime_configuration();
        configuration.logging.file = uncreatable_log_path();
        CHECK_THROWS_AS(avis::runtime{ configuration }, avis::runtime_error);
        CHECK_FALSE(avis::log::detail::initialised());
    }

    CHECK_FALSE(avis::runtime::alive());
    CHECK(avis::tests::current_process_thread_count() == before);

    // A new construction succeeds after the failure
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    CHECK(avis::runtime::alive());
}

TEST_CASE("static accessors work only while a runtime is alive", "[runtime]")
{
    const avis::tests::throwing_runtime_accessors throwing;

    CHECK_FALSE(avis::runtime::alive());
    CHECK_THROWS_AS(avis::runtime::thread_pool(), std::logic_error);
    CHECK_THROWS_AS(avis::runtime::io_context(), std::logic_error);
    CHECK_THROWS_AS(avis::runtime::configuration(), std::logic_error);
    avis::log::get("core")->info("dropped: no runtime, so no output sinks");

    {
        avis::runtime runtime{ avis::tests::small_runtime_configuration() };
        CHECK(avis::runtime::alive());
        CHECK(avis::runtime::thread_pool().running() == 1); // the IO thread
        CHECK(avis::runtime::configuration().io.threads == 1);

        auto on_io = stdexec::sync_wait(
            stdexec::schedule(avis::runtime::io_context().get_scheduler()) |
            stdexec::then([] { return avis::io::io_context::current(); }));
        REQUIRE(on_io.has_value());
        CHECK(std::get<0>(*on_io) == &avis::runtime::io_context());
    }

    CHECK_FALSE(avis::runtime::alive());
    CHECK_THROWS_AS(avis::runtime::thread_pool(), std::logic_error);
    CHECK_THROWS_AS(avis::runtime::io_context(), std::logic_error);
    CHECK_THROWS_AS(avis::runtime::configuration(), std::logic_error);
    avis::log::get("core")->info("dropped again");
}

TEST_CASE("the parallel scheduler works exactly while a runtime is alive", "[runtime]")
{
    CHECK_THROWS_AS(stdexec::get_parallel_scheduler(), std::runtime_error);

    {
        avis::runtime runtime{ avis::tests::small_runtime_configuration() };
        auto scheduler = stdexec::get_parallel_scheduler();

        std::set<std::thread::id> threads;
        for (int run = 0; run < 100; ++run)
        {
            auto result = stdexec::sync_wait(
                stdexec::schedule(scheduler) | stdexec::then([] { return std::this_thread::get_id(); }));
            REQUIRE(result.has_value());
            threads.insert(std::get<0>(*result));
        }
        CHECK_FALSE(threads.contains(std::this_thread::get_id()));
        CHECK(threads.size() <= avis::runtime::configuration().parallel.workers);
    }

    CHECK_THROWS_AS(stdexec::get_parallel_scheduler(), std::runtime_error);
}

TEST_CASE("a parallel scheduler kept past the runtime completes with set_stopped", "[runtime]")
{
    std::optional<stdexec::parallel_scheduler> kept;
    {
        avis::runtime runtime{ avis::tests::small_runtime_configuration() };
        kept = stdexec::get_parallel_scheduler();
        REQUIRE(stdexec::sync_wait(stdexec::schedule(*kept)).has_value());
    }

    CHECK_FALSE(stdexec::sync_wait(stdexec::schedule(*kept)).has_value());
}

TEST_CASE("the resolved configuration is available", "[runtime]")
{
    auto configuration = avis::tests::small_runtime_configuration();
    configuration.parallel.workers = 0;
    configuration.thread_pool.capacity = 0;

    avis::runtime runtime{ configuration };
    const auto& resolved = avis::runtime::configuration();
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    CHECK(resolved.parallel.workers == (hardware_threads > 2 ? hardware_threads - 2 : 1));
    CHECK(resolved.thread_pool.capacity == 1 + 1 + 2); // io + application threads + headroom
    CHECK(avis::runtime::thread_pool().capacity() == resolved.thread_pool.capacity);
}

TEST_CASE("parallel work that issues IO finishes before the runtime shuts down", "[runtime]")
{
    const std::size_t before = avis::tests::current_process_thread_count();
    {
        avis::runtime runtime{ avis::tests::small_runtime_configuration() };

        auto work = stdexec::schedule(stdexec::get_parallel_scheduler()) |
                    stdexec::let_value(
                        []
                        {
                            return stdexec::schedule(avis::runtime::io_context().get_scheduler()) |
                                   stdexec::then([] { return avis::io::io_context::current() != nullptr; });
                        });

        // The test waits for the work before destroying the runtime: queued work isn't required to finish (D-63)
        auto result = avis::threading::sync_wait(std::move(work));
        REQUIRE(result.has_value());
        CHECK(std::get<0>(*result));
    }
    CHECK(avis::tests::current_process_thread_count() == before);
}

TEST_CASE("a forgotten application thread is stopped by the runtime's backstop", "[runtime]")
{
    avis::tests::log_capture capture{ "core" };
    const std::size_t before = avis::tests::current_process_thread_count();

    avis::threading::thread_handle forgotten;
    {
        avis::runtime runtime{ avis::tests::small_runtime_configuration() };
        auto started = avis::runtime::thread_pool().start("forgotten", wait_for_stop);
        REQUIRE(started.has_value());
        forgotten = std::move(*started); // outlives the runtime
    }

    CHECK_FALSE(forgotten.running());
    CHECK_FALSE(capture.contains("threads still running"));
    CHECK(avis::tests::current_process_thread_count() == before);
    forgotten = avis::threading::thread_handle{};
}

namespace
{
    // The shape of an application's main
    int run_application()
    {
        std::atomic<int> ticks = 0;
        auto simulation = avis::runtime::thread_pool().start(
            "simulation",
            [&ticks](const std::stop_token& token)
            {
                while (!token.stop_requested() && ticks.load() < 3)
                {
                    ticks.fetch_add(1);
                }
            });
        if (!simulation)
        {
            return 1;
        }
        simulation->join(); // app-owned threads stopped before returning
        return ticks.load() == 3 ? 0 : 1;
    }

    int application_main()
    {
        auto configuration = avis::runtime_configuration::parse("runtime: { io_threads: 1 }\nlogging: { level: warn }");
        if (!configuration)
        {
            return 2;
        }
        configuration->application_threads = 1;
        configuration->logging.debugger_output = false;

        avis::runtime runtime{ *configuration };
        return run_application();
    }
} // namespace

TEST_CASE("the runtime at the top of main", "[runtime]")
{
    CHECK(application_main() == 0);
    CHECK_FALSE(avis::runtime::alive());
}

TEST_CASE_METHOD(avis::tests::runtime_fixture, "the fixture keeps a small runtime alive", "[runtime]")
{
    CHECK(avis::runtime::alive());
    CHECK(avis::runtime::configuration().parallel.workers == 2);
    CHECK(stdexec::sync_wait(stdexec::schedule(stdexec::get_parallel_scheduler())).has_value());
}
