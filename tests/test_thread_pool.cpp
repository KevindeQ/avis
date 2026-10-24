#include "helpers/allocation_failure.h"
#include "helpers/log_capture.h"
#include "helpers/thread_count.h"

#include "avis/threading/thread_pool.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <latch>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

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

    avis::threading::thread_handle start_or_fail(
        avis::threading::thread_pool& pool, const std::string_view name, avis::threading::thread_body body)
    {
        auto handle = pool.start(name, std::move(body));
        REQUIRE(handle.has_value());
        return std::move(*handle);
    }

    std::int32_t code_of(const avis::error& error)
    {
        return error.code;
    }
} // namespace

TEST_CASE("a body runs until stop is requested", "[thread_pool]")
{
    avis::threading::thread_pool pool{ { .capacity = 4 } };

    auto handle = start_or_fail(pool, "waiter", wait_for_stop);
    CHECK(handle.name() == "waiter");
    CHECK(handle.running());
    CHECK(pool.running() == 1);

    pool.request_stop();
    pool.join();

    CHECK_FALSE(handle.running());
    CHECK_FALSE(handle.exit_error().has_value());
    CHECK(pool.running() == 0);
}

TEST_CASE("destroying a handle stops and joins its body only", "[thread_pool]")
{
    avis::threading::thread_pool pool{ { .capacity = 4 } };

    auto kept = start_or_fail(pool, "kept", wait_for_stop);
    {
        auto dropped = start_or_fail(pool, "dropped", wait_for_stop);
        CHECK(pool.running() == 2);
    }

    CHECK(pool.running() == 1);
    CHECK(kept.running());
}

TEST_CASE("capacity is enforced and slots are reused", "[thread_pool]")
{
    avis::threading::thread_pool pool{ { .capacity = 2 } };

    auto first = start_or_fail(pool, "first", wait_for_stop);
    auto second = start_or_fail(pool, "second", wait_for_stop);

    auto third = pool.start("third", wait_for_stop);
    REQUIRE_FALSE(third.has_value());
    CHECK(code_of(third.error()) == static_cast<std::int32_t>(avis::generic_error_kind::pool_exhausted));

    first.request_stop();
    first.join();

    auto replacement = pool.start("replacement", wait_for_stop);
    CHECK(replacement.has_value());
}

TEST_CASE("start fails once the pool is stopping", "[thread_pool]")
{
    avis::threading::thread_pool pool{ { .capacity = 2 } };
    pool.request_stop();

    auto handle = pool.start("late", wait_for_stop);
    REQUIRE_FALSE(handle.has_value());
    CHECK(code_of(handle.error()) == static_cast<std::int32_t>(avis::generic_error_kind::pool_stopping));
}

TEST_CASE("an exception escaping a body is contained", "[thread_pool]")
{
    avis::tests::log_capture capture{ "core" };
    avis::threading::thread_pool pool{ { .capacity = 4 } };

    auto survivor = start_or_fail(pool, "survivor", wait_for_stop);

    std::promise<std::optional<avis::error>> reported;
    avis::threading::thread_options options;
    options.on_exit = [&reported](const std::optional<avis::error>& error) { reported.set_value(error); };

    auto thrower =
        pool.start("thrower", [](std::stop_token) { throw std::runtime_error{ "body failed" }; }, std::move(options));
    REQUIRE(thrower.has_value());

    const auto error = reported.get_future().get();
    REQUIRE(error.has_value());
    CHECK(error->message == "body failed");

    thrower->join();
    REQUIRE(thrower->exit_error().has_value());
    CHECK(thrower->exit_error()->message == "body failed");
    CHECK(capture.contains("avis-thrower"));

    CHECK(survivor.running());
}

TEST_CASE("exit_error is a copy that is safe to read at any time", "[thread_pool]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };

    std::latch started{ 1 };
    std::latch release{ 1 };
    auto handle = pool.start(
        "failing",
        [&](std::stop_token)
        {
            started.count_down();
            release.wait();
            throw std::runtime_error{ "late failure" };
        });
    REQUIRE(handle.has_value());

    // While the body runs there is no error yet
    started.wait();
    CHECK_FALSE(handle->exit_error().has_value());

    release.count_down();
    handle->join();
    const std::optional<avis::error> error = handle->exit_error();

    // The copy stays valid after the handle has released the thread
    *handle = avis::threading::thread_handle{};
    REQUIRE(error.has_value());
    CHECK(error->message == "late failure");
    CHECK_FALSE(handle->exit_error().has_value());
}

TEST_CASE("the slot is free again when on_exit runs", "[thread_pool]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };

    std::promise<bool> restarted;
    avis::threading::thread_handle restart;

    avis::threading::thread_options options;
    options.on_exit = [&](const std::optional<avis::error>&)
    {
        auto handle = pool.start("restart", wait_for_stop);
        restarted.set_value(handle.has_value());
        if (handle)
        {
            restart = std::move(*handle);
        }
    };

    auto first = pool.start("first", [](std::stop_token) {}, std::move(options));
    REQUIRE(first.has_value());
    CHECK(restarted.get_future().get());
}

TEST_CASE("request_stop signals every body before join waits", "[thread_pool]")
{
    constexpr std::size_t body_count = 4;

    std::mutex mutex;
    std::vector<std::chrono::steady_clock::time_point> stop_observed;
    std::vector<std::chrono::steady_clock::time_point> exited;

    std::vector<avis::threading::thread_handle> handles;
    std::optional<avis::threading::thread_pool> pool{ avis::threading::thread_pool_configuration{ .capacity =
                                                                                                      body_count } };

    std::latch started{ body_count };
    for (std::size_t index = 0; index < body_count; ++index)
    {
        handles.push_back(start_or_fail(
            *pool,
            "body",
            [&](std::stop_token token)
            {
                started.count_down();
                wait_for_stop(token);
                {
                    std::lock_guard lock{ mutex };
                    stop_observed.push_back(std::chrono::steady_clock::now());
                }

                std::this_thread::sleep_for(50ms);

                std::lock_guard lock{ mutex };
                exited.push_back(std::chrono::steady_clock::now());
            }));
    }
    started.wait();

    pool.reset();

    REQUIRE(stop_observed.size() == body_count);
    REQUIRE(exited.size() == body_count);
    CHECK(std::ranges::max(stop_observed) < std::ranges::min(exited));
}

TEST_CASE("join logs threads that ignore the stop request", "[thread_pool]")
{
    avis::tests::log_capture capture{ "core" };
    {
        avis::threading::thread_pool pool{ { .capacity = 1, .join_warning = 50ms } };
        auto handle = pool.start("stubborn", [](std::stop_token) { std::this_thread::sleep_for(200ms); });
        REQUIRE(handle.has_value());

        pool.request_stop();
        pool.join();
        CHECK_FALSE(handle->running());
    }

    CHECK(capture.contains("threads still running 50 ms after the stop request: avis-stubborn"));
}

TEST_CASE("current_index is dense, unique and reused", "[thread_pool]")
{
    CHECK_FALSE(avis::threading::thread_pool::current_index().has_value());

    avis::threading::thread_pool pool{ { .capacity = 3 } };

    // indices[n] is the pool index seen by the body of handles[n]
    std::array<std::uint32_t, 3> indices{};
    std::latch started{ 3 };

    std::vector<avis::threading::thread_handle> handles;
    for (std::size_t position = 0; position < indices.size(); ++position)
    {
        handles.push_back(start_or_fail(
            pool,
            "indexed",
            [&indices, &started, position](std::stop_token token)
            {
                indices[position] = avis::threading::thread_pool::current_index().value();
                started.count_down();
                wait_for_stop(token);
            }));
    }
    started.wait();

    CHECK(std::set<std::uint32_t>(indices.begin(), indices.end()) == std::set<std::uint32_t>{ 0, 1, 2 });

    // Stop the thread that got index 1; the next thread gets it again
    const auto position = std::ranges::find(indices, 1u) - indices.begin();
    handles[static_cast<std::size_t>(position)] = avis::threading::thread_handle{};

    std::promise<std::optional<std::uint32_t>> reused;
    auto replacement = start_or_fail(
        pool,
        "replacement",
        [&reused](std::stop_token token)
        {
            reused.set_value(avis::threading::thread_pool::current_index());
            wait_for_stop(token);
        });

    CHECK(reused.get_future().get() == 1u);
}

TEST_CASE("pool threads are named avis-<name>", "[thread_pool]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };

    std::promise<std::wstring> description;
    auto handle = start_or_fail(
        pool,
        "named",
        [&description](std::stop_token)
        {
            PWSTR text = nullptr;
            std::wstring value;
            if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &text)))
            {
                value = text;
                LocalFree(text);
            }
            description.set_value(value);
        });

    CHECK(description.get_future().get() == L"avis-named");
}

TEST_CASE("a handle may outlive its pool", "[thread_pool]")
{
    avis::threading::thread_handle handle;
    {
        avis::threading::thread_pool pool{ { .capacity = 1 } };
        handle = start_or_fail(pool, "outliving", wait_for_stop);
    }

    CHECK_FALSE(handle.running());
    handle.join();
    handle = avis::threading::thread_handle{};
}

TEST_CASE("no threads remain after the pool is destroyed", "[thread_pool]")
{
    const std::size_t before = avis::tests::current_process_thread_count();
    {
        avis::threading::thread_pool pool{ { .capacity = 4 } };
        std::vector<avis::threading::thread_handle> handles;
        for (int index = 0; index < 4; ++index)
        {
            handles.push_back(start_or_fail(pool, "counted", wait_for_stop));
        }
        CHECK(avis::tests::current_process_thread_count() == before + 4);

        // The handles stay alive: the pool's destructor is the backstop that stops and joins them
        pool.request_stop();
        pool.join();
    }

    CHECK(avis::tests::current_process_thread_count() == before);
}

TEST_CASE("an empty body is rejected", "[thread_pool]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };

    auto empty = pool.start("empty", avis::threading::thread_body{});
    REQUIRE_FALSE(empty.has_value());
    CHECK(code_of(empty.error()) == static_cast<std::int32_t>(avis::generic_error_kind::logic_error));
    CHECK(pool.running() == 0);

    auto valid = pool.start("valid", wait_for_stop);
    CHECK(valid.has_value());
}

TEST_CASE("a thread that replaces its own handle from on_exit is reclaimed", "[thread_pool]")
{
    constexpr int restarts = 64;

    avis::tests::log_capture capture{ "core" };
    avis::threading::thread_pool pool{ { .capacity = 2 } };

    // Restarts its thread from on_exit by assigning the new handle over the exiting thread's own handle. The mutex
    // makes each thread finish storing its successor's handle before that successor replaces it.
    struct restarter
    {
        avis::threading::thread_pool& pool;
        int remaining = restarts;
        bool failed = false;
        std::promise<void> done;
        std::mutex mutex;
        avis::threading::thread_handle handle; // last member: destroyed (and joined) first

        // Requires mutex
        void start()
        {
            avis::threading::thread_options options;
            options.on_exit = [this](const std::optional<avis::error>&)
            {
                std::lock_guard lock{ mutex };
                if (--remaining == 0)
                {
                    done.set_value();
                    return;
                }
                start();
            };

            auto started = pool.start("restarting", [](std::stop_token) {}, std::move(options));
            if (!started)
            {
                failed = true;
                done.set_value();
                return;
            }
            handle = std::move(*started);
        }
    };

    const std::size_t handles_before = avis::tests::current_process_handle_count();
    bool failed = false;
    {
        restarter chain{ .pool = pool };
        {
            std::lock_guard lock{ chain.mutex };
            chain.start();
        }
        chain.done.get_future().get();
        failed = chain.failed;
    }
    const std::size_t handles_after = avis::tests::current_process_handle_count();

    REQUIRE_FALSE(failed);

    // Without reclaiming, each restart would keep the handle of an exited, never-joined OS thread open
    CHECK(handles_after < handles_before + restarts / 4);
    CHECK_FALSE(capture.contains("tried to join itself"));
}

TEST_CASE("start fails cleanly when the pool can't grow its thread list", "[thread_pool]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    const std::size_t threads_before = avis::tests::current_process_thread_count();

    // A fresh pool has an empty thread list, so the first allocation of start() is the one that grows it
    bool triggered = false;
    auto failed = [&]
    {
        avis::tests::scoped_allocation_failure failure;
        auto result = pool.start("unlucky", wait_for_stop);
        triggered = failure.triggered();
        return result;
    }();

    REQUIRE(triggered);
    REQUIRE_FALSE(failed.has_value());
    CHECK(pool.running() == 0);
    CHECK(avis::tests::current_process_thread_count() == threads_before); // no untracked thread was started

    auto retried = pool.start("retried", wait_for_stop);
    CHECK(retried.has_value());
}

TEST_CASE("start frees the slot when the thread object can't be allocated", "[thread_pool]")
{
    avis::threading::thread_pool pool{ { .capacity = 1 } };

    // Leaves room in the thread list, so the first allocation of the next start() is the thread object itself
    {
        auto first = start_or_fail(pool, "first", wait_for_stop);
    }

    bool triggered = false;
    auto failed = [&]
    {
        avis::tests::scoped_allocation_failure failure;
        auto result = pool.start("unlucky", wait_for_stop);
        triggered = failure.triggered();
        return result;
    }();

    REQUIRE(triggered);
    REQUIRE_FALSE(failed.has_value());
    CHECK(pool.running() == 0);

    // With capacity 1, this only succeeds if the failed start gave its slot back
    auto retried = pool.start("retried", wait_for_stop);
    CHECK(retried.has_value());
}
