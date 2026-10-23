// Tests for the parallel scheduler backend. The stress test and the benchmark are hidden from the default run:
//   avis_tests "[stress]"       (duration: AVIS_STRESS_SECONDS, default 10)
//   avis_tests "[benchmark]"

#include "helpers/runtime_fixture.h"
#include "helpers/thread_count.h"

#include "avis/io/asio_scheduler.h"
#include "avis/threading/parallel_helpers.h"

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <exec/static_thread_pool.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    // Receiver that reports which channel completed, with an environment of choice
    template<typename Environment = stdexec::env<>>
    struct channel_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        template<typename... Values>
        void set_value(Values&&...) noexcept
        {
            result->set_value("value");
        }

        template<typename Error>
        void set_error(Error&&) noexcept
        {
            result->set_value("error");
        }

        void set_stopped() noexcept
        {
            result->set_value("stopped");
        }

        auto get_env() const noexcept -> Environment
        {
            return environment;
        }

        std::promise<std::string>* result;
        Environment environment{};
    };

    // Owns a connected operation, so it can be started from anywhere and outlive the starting scope
    template<typename Sender, typename Receiver>
    struct operation_holder
    {
        operation_holder(Sender sender, Receiver receiver) :
            operation{ stdexec::connect(std::move(sender), std::move(receiver)) }
        {}

        stdexec::connect_result_t<Sender, Receiver> operation;
    };

    auto stop_environment(const stdexec::inplace_stop_token token)
    {
        return stdexec::prop{ stdexec::get_stop_token, token };
    }

    std::chrono::seconds stress_duration()
    {
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "AVIS_STRESS_SECONDS") == 0 && value != nullptr)
        {
            const int seconds = std::atoi(value);
            std::free(value);
            if (seconds > 0)
            {
                return std::chrono::seconds{ seconds };
            }
        }
        return 10s;
    }
} // namespace

static_assert(stdexec::scheduler<stdexec::parallel_scheduler>);

TEST_CASE("get_parallel_scheduler runs work on the runtime's pool", "[parallel]")
{
    CHECK_THROWS_AS(stdexec::get_parallel_scheduler(), std::runtime_error);

    {
        constexpr std::uint32_t workers = 3;
        avis::runtime runtime{ avis::tests::small_runtime_configuration(workers) };
        auto scheduler = stdexec::get_parallel_scheduler();

        CHECK(stdexec::get_forward_progress_guarantee(scheduler) == stdexec::forward_progress_guarantee::parallel);

        std::set<std::thread::id> threads;
        for (int run = 0; run < 200; ++run)
        {
            auto result = stdexec::sync_wait(
                stdexec::schedule(scheduler) | stdexec::then([] { return std::this_thread::get_id(); }));
            REQUIRE(result.has_value());
            threads.insert(std::get<0>(*result));
        }

        CHECK_FALSE(threads.contains(std::this_thread::get_id()));
        CHECK(threads.size() <= workers);
    }

    CHECK_THROWS_AS(stdexec::get_parallel_scheduler(), std::runtime_error);
}

TEST_CASE("bulk on the parallel scheduler covers every index exactly once", "[parallel][bulk]")
{
    constexpr std::uint32_t workers = 4;
    avis::runtime runtime{ avis::tests::small_runtime_configuration(workers) };
    auto scheduler = stdexec::get_parallel_scheduler();
    const std::thread::id test_thread = std::this_thread::get_id();

    // 3 × workers is the chunking threshold of the backend
    for (const std::size_t size : { std::size_t{ 0 },
                                    std::size_t{ 1 },
                                    std::size_t{ 7 },
                                    std::size_t{ 3 * workers },
                                    std::size_t{ 3 * workers + 1 },
                                    std::size_t{ 1'000'000 } })
    {
        INFO("size " << size);
        auto hits = std::make_unique<std::atomic<std::uint8_t>[]>(size + 1);
        std::atomic<std::size_t> off_workers = 0;
        std::atomic<std::size_t> unmarked = 0;

        auto work = stdexec::schedule(scheduler) | stdexec::bulk(
                                                       stdexec::par,
                                                       size,
                                                       [&](const std::size_t index)
                                                       {
                                                           hits[index].fetch_add(1, std::memory_order_relaxed);
                                                           if (std::this_thread::get_id() == test_thread)
                                                           {
                                                               off_workers.fetch_add(1, std::memory_order_relaxed);
                                                           }
                                                           if (!avis::threading::detail::in_parallel_work())
                                                           {
                                                               unmarked.fetch_add(1, std::memory_order_relaxed);
                                                           }
                                                       });
        REQUIRE(stdexec::sync_wait(std::move(work)).has_value());

        std::size_t wrong = 0;
        for (std::size_t index = 0; index < size; ++index)
        {
            wrong += hits[index].load() != 1 ? 1 : 0;
        }
        CHECK(wrong == 0);
        CHECK(off_workers.load() == 0);
        CHECK(unmarked.load() == 0);
    }
}

TEST_CASE("bulk_chunked ranges are contiguous and cover the shape", "[parallel][bulk]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration(4) };
    auto scheduler = stdexec::get_parallel_scheduler();

    for (const std::size_t size : { std::size_t{ 1 }, std::size_t{ 12 }, std::size_t{ 13 }, std::size_t{ 100'000 } })
    {
        INFO("size " << size);
        std::mutex mutex;
        std::vector<std::pair<std::size_t, std::size_t>> ranges;

        auto work = stdexec::schedule(scheduler) | stdexec::bulk_chunked(
                                                       stdexec::par,
                                                       size,
                                                       [&](const std::size_t begin, const std::size_t end)
                                                       {
                                                           std::lock_guard lock{ mutex };
                                                           ranges.emplace_back(begin, end);
                                                       });
        REQUIRE(stdexec::sync_wait(std::move(work)).has_value());

        std::ranges::sort(ranges);
        REQUIRE_FALSE(ranges.empty());
        CHECK(ranges.front().first == 0);
        CHECK(ranges.back().second == size);
        bool contiguous = true;
        for (std::size_t index = 0; index < ranges.size(); ++index)
        {
            contiguous = contiguous && ranges[index].first < ranges[index].second;
            if (index > 0)
            {
                contiguous = contiguous && ranges[index - 1].second == ranges[index].first;
            }
        }
        CHECK(contiguous);
    }
}

TEST_CASE("an exception in bulk completes with one error", "[parallel][bulk]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration(4) };
    auto scheduler = stdexec::get_parallel_scheduler();

    auto failing_bulk = [&]
    {
        return stdexec::schedule(scheduler) | stdexec::bulk(
                                                  stdexec::par,
                                                  1'000,
                                                  [](const std::size_t index)
                                                  {
                                                      if (index == 5)
                                                      {
                                                          throw std::runtime_error{ "bulk failed" };
                                                      }
                                                  });
    };

    // Without conversion the exception itself comes back
    CHECK_THROWS_WITH(stdexec::sync_wait(failing_bulk()), "bulk failed");

    // With into_internal_error it's an avis::error carrying the message
    try
    {
        stdexec::sync_wait(failing_bulk() | avis::threading::into_internal_error());
        FAIL("bulk should have completed with an error");
    }
    catch (const avis::error& error)
    {
        CHECK(error.message == "bulk failed");
        CHECK(error.code == static_cast<std::int32_t>(avis::generic_errc::unhandled_exception));
    }
}

TEST_CASE("into_internal_error converts exceptions from then", "[parallel]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration(1) };

    auto work = stdexec::schedule(stdexec::get_parallel_scheduler()) |
                stdexec::then([]() -> int { throw std::runtime_error{ "parse failed" }; }) |
                avis::threading::into_internal_error();

    try
    {
        stdexec::sync_wait(std::move(work));
        FAIL("the pipeline should have completed with an error");
    }
    catch (const avis::error& error)
    {
        CHECK(error.message == "parse failed");
    }
}

TEST_CASE("a stop request before the work runs completes with set_stopped", "[parallel]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration(1) };

    stdexec::inplace_stop_source source;
    source.request_stop();

    std::promise<std::string> result;
    using receiver_type = channel_receiver<decltype(stop_environment(source.get_token()))>;
    operation_holder holder{ stdexec::schedule(stdexec::get_parallel_scheduler()),
                             receiver_type{ &result, stop_environment(source.get_token()) } };
    stdexec::start(holder.operation);

    CHECK(result.get_future().get() == "stopped");
}

TEST_CASE("schedulers kept past the runtime complete with set_stopped", "[parallel]")
{
    std::optional<stdexec::parallel_scheduler> kept;
    {
        avis::runtime runtime{ avis::tests::small_runtime_configuration(2) };
        kept = stdexec::get_parallel_scheduler();
        REQUIRE(stdexec::sync_wait(stdexec::schedule(*kept)).has_value());
    } // shutdown detaches the backend, then destroys the pool
    auto& scheduler = *kept;

    CHECK_FALSE(stdexec::sync_wait(stdexec::schedule(scheduler)).has_value());
    CHECK_FALSE(
        stdexec::sync_wait(stdexec::schedule(scheduler) | stdexec::bulk(stdexec::par, 100, [](std::size_t) {}))
            .has_value());
}

TEST_CASE("avis::threading::sync_wait refuses to block parallel work", "[parallel]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration(2) };
    auto scheduler = stdexec::get_parallel_scheduler();

    auto nested = stdexec::schedule(scheduler) |
                  stdexec::then([] { return avis::threading::sync_wait(stdexec::just(1)).has_value(); });
    CHECK_THROWS_AS(stdexec::sync_wait(std::move(nested)), std::logic_error);

    // Outside parallel work it waits as usual
    CHECK_FALSE(avis::threading::detail::in_parallel_work());
    CHECK(avis::threading::sync_wait(stdexec::schedule(scheduler)).has_value());
}

TEST_CASE("the parallel scheduler design examples run", "[parallel]")
{
    asio::io_context io_context;
    auto guard = asio::make_work_guard(io_context);
    std::jthread io_thread{ [&io_context] { io_context.run(); } };
    const std::thread::id io_thread_id = io_thread.get_id();

    avis::runtime runtime{ avis::tests::small_runtime_configuration(2) };
    auto parallel = stdexec::get_parallel_scheduler();

    // Render graph: record groups in parallel, composed into one sender
    const std::vector<int> groups(64, 1);
    std::atomic<int> recorded = 0;
    auto record =
        stdexec::schedule(parallel) |
        stdexec::bulk(stdexec::par, groups.size(), [&](const std::size_t index) { recorded.fetch_add(groups[index]); });
    REQUIRE(stdexec::sync_wait(std::move(record)).has_value());
    CHECK(recorded.load() == 64);

    // IO, then decode on parallel workers (an io::scheduler sender stands in for avis::io::read_file)
    avis::io::scheduler io_scheduler{ io_context };
    auto image = stdexec::schedule(io_scheduler) | stdexec::then([] { return std::vector<std::byte>(16); }) |
                 stdexec::continues_on(parallel) |
                 stdexec::then(
                     [](std::vector<std::byte> bytes)
                     {
                         return std::tuple{ bytes.size(),
                                            std::this_thread::get_id(),
                                            avis::threading::detail::in_parallel_work() };
                     }) |
                 avis::threading::into_internal_error();

    auto result = avis::threading::sync_wait(std::move(image));
    REQUIRE(result.has_value());
    const auto [size, thread, marked] = std::get<0>(*result);
    CHECK(size == 16);
    CHECK(thread != io_thread_id);
    CHECK(thread != std::this_thread::get_id());
    CHECK(marked);

    guard.reset();
}

TEST_CASE("no parallel workers remain after shutdown", "[parallel]")
{
    const std::size_t before = avis::tests::current_process_thread_count();
    {
        avis::runtime runtime{ avis::tests::small_runtime_configuration(4) };
        REQUIRE(stdexec::sync_wait(stdexec::schedule(stdexec::get_parallel_scheduler())).has_value());
        CHECK(avis::tests::current_process_thread_count() == before + 1 + 4); // IO thread + parallel workers
    }
    CHECK(avis::tests::current_process_thread_count() == before);
}

TEST_CASE("parallel scheduler under mixed load", "[.][stress]")
{
    constexpr int producer_count = 4;
    avis::runtime runtime{ avis::tests::small_runtime_configuration(4) };
    auto scheduler = stdexec::get_parallel_scheduler();

    std::atomic<bool> deadline_reached = false;
    std::atomic<std::uint64_t> values = 0;
    std::atomic<std::uint64_t> stopped = 0;
    std::atomic<std::uint64_t> wrong_bulks = 0;
    stdexec::counting_scope spawned;

    std::vector<std::jthread> producers;
    for (int producer = 0; producer < producer_count; ++producer)
    {
        producers.emplace_back(
            [&, producer]
            {
                std::mt19937 random{ static_cast<std::uint32_t>(producer) };
                std::uniform_int_distribution<int> pick{ 0, 2 };
                std::uniform_int_distribution<std::size_t> bulk_size{ 0, 5'000 };

                while (!deadline_reached.load(std::memory_order_acquire))
                {

                    switch (pick(random))
                    {
                    case 0:
                        stdexec::spawn(
                            stdexec::schedule(scheduler) |
                                stdexec::then([&values]() noexcept { values.fetch_add(1); }) |
                                stdexec::upon_error([](auto&&) noexcept {}) |
                                stdexec::upon_stopped([&stopped]() noexcept { stopped.fetch_add(1); }),
                            spawned.get_token());
                        break;
                    case 1:
                        {
                            const std::size_t size = bulk_size(random);
                            std::atomic<std::size_t> covered = 0;
                            auto result = stdexec::sync_wait(
                                stdexec::schedule(scheduler) |
                                stdexec::bulk(stdexec::par, size, [&covered](std::size_t) { covered.fetch_add(1); }));
                            if (!result)
                            {
                                stopped.fetch_add(1);
                            }
                            else if (covered.load() != size)
                            {
                                wrong_bulks.fetch_add(1);
                            }
                            else
                            {
                                values.fetch_add(1);
                            }
                            break;
                        }
                    default:
                        {
                            auto result = stdexec::sync_wait(
                                stdexec::schedule(scheduler) | stdexec::then([] { return 1; }) |
                                stdexec::continues_on(scheduler) | stdexec::then([](int value) { return value + 1; }));
                            if (result)
                            {
                                values.fetch_add(1);
                            }
                            else
                            {
                                stopped.fetch_add(1);
                            }
                            break;
                        }
                    }
                }
            });
    }

    std::this_thread::sleep_for(stress_duration());
    deadline_reached.store(true, std::memory_order_release);

    producers.clear(); // joins
    stdexec::sync_wait(spawned.join());

    // The runtime outlived every submission, so every operation completed with a value
    CHECK(values.load() > 0);
    CHECK(stopped.load() == 0);
    CHECK(wrong_bulks.load() == 0);
}

TEST_CASE("parallel scheduler throughput", "[.][benchmark]")
{
    constexpr std::uint64_t task_count = 100'000;
    constexpr std::size_t bulk_size = 1'000'000;

    avis::runtime runtime{ avis::tests::small_runtime_configuration(4) };
    auto parallel = stdexec::get_parallel_scheduler();
    exec::static_thread_pool reference{ 4 }; // the same pool type, reached without type erasure
    auto direct = reference.get_scheduler();

    // clang-format off: clang-format would drop the semicolons that end the BENCHMARK blocks
    BENCHMARK("get_parallel_scheduler spawn schedule() from outside, 100k")
    {
        std::atomic<std::uint64_t> done = 0;
        stdexec::counting_scope spawned;
        for (std::uint64_t index = 0; index < task_count; ++index)
        {
            stdexec::spawn(stdexec::schedule(parallel) | stdexec::then([&done]() noexcept { done.fetch_add(1); }) |
                               stdexec::upon_error([](auto&&) noexcept {}),
                           spawned.get_token());
        }
        stdexec::sync_wait(spawned.join());
        return done.load();
    };

    BENCHMARK("static_thread_pool spawn schedule() from outside, 100k")
    {
        std::atomic<std::uint64_t> done = 0;
        stdexec::counting_scope spawned;
        for (std::uint64_t index = 0; index < task_count; ++index)
        {
            stdexec::spawn(stdexec::schedule(direct) | stdexec::then([&done]() noexcept { done.fetch_add(1); }) |
                               stdexec::upon_error([](auto&&) noexcept {}),
                           spawned.get_token());
        }
        stdexec::sync_wait(spawned.join());
        return done.load();
    };

    BENCHMARK("get_parallel_scheduler bulk over 1M indices")
    {
        std::atomic<std::uint64_t> sum = 0;
        stdexec::sync_wait(stdexec::schedule(parallel) |
                           stdexec::bulk(stdexec::par, bulk_size, [&sum](std::size_t) { sum.fetch_add(1, std::memory_order_relaxed); }));
        return sum.load();
    };

    BENCHMARK("static_thread_pool bulk over 1M indices")
    {
        std::atomic<std::uint64_t> sum = 0;
        stdexec::sync_wait(stdexec::schedule(direct) |
                           stdexec::bulk(stdexec::par, bulk_size, [&sum](std::size_t) { sum.fetch_add(1, std::memory_order_relaxed); }));
        return sum.load();
    };
    // clang-format on
}
