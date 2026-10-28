// The leaf-lock check of every RHI mutex (docs/tasks/06-rhi.md §6.3.2, docs/design/09-rhi.md §19.4). No device.

#include "avis/detail/check.h"
#include "avis/rhi/d3d12/leaf_mutex.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

using avis::rhi::d3d12::leaf_mutex;

#ifdef NDEBUG

TEST_CASE("leaf_mutex is a plain std::mutex in release builds", "[rhi][unit]")
{
    static_assert(std::is_base_of_v<std::mutex, leaf_mutex>);
    static_assert(sizeof(leaf_mutex) == sizeof(std::mutex));

    leaf_mutex first{ "first" };
    leaf_mutex second{ "second" };
    const std::scoped_lock both{ first, second }; // nesting isn't checked in release builds
    SUCCEED();
}

#else

namespace
{
    std::vector<std::string>& recorded() noexcept
    {
        static std::vector<std::string> messages;
        return messages;
    }

    void recording_handler(const std::string_view message, std::source_location) noexcept
    {
        recorded().emplace_back(message);
    }

    class recording_scope
    {
    public:
        recording_scope() : previous_{ avis::detail::set_assertion_handler(&recording_handler) }
        {
            recorded().clear();
        }

        ~recording_scope()
        {
            avis::detail::set_assertion_handler(previous_);
            recorded().clear();
        }

        recording_scope(const recording_scope&) = delete;
        recording_scope& operator=(const recording_scope&) = delete;

    private:
        avis::detail::assertion_handler previous_;
    };
} // namespace

TEST_CASE("leaf_mutex reports a nested RHI lock in debug builds", "[rhi][unit]")
{
    const recording_scope handler;
    leaf_mutex outer{ "outer" };
    leaf_mutex inner{ "inner" };

    SECTION("lock inside lock")
    {
        {
            const std::lock_guard outer_lock{ outer };
            CHECK(leaf_mutex::held_by_this_thread());
            const std::lock_guard inner_lock{ inner };
        }
        REQUIRE(recorded().size() == 1);
        CHECK(recorded().front().contains("'inner'"));
        CHECK_FALSE(leaf_mutex::held_by_this_thread());
    }

    SECTION("try_lock inside lock")
    {
        {
            const std::lock_guard outer_lock{ outer };
            REQUIRE(inner.try_lock());
            inner.unlock();
        }
        REQUIRE(recorded().size() == 1);
        CHECK(recorded().front().contains("'inner'"));
    }

    SECTION("sequential locks are fine")
    {
        {
            const std::lock_guard outer_lock{ outer };
        }
        {
            const std::unique_lock inner_lock{ inner };
        }
        CHECK(recorded().empty());
        CHECK_FALSE(leaf_mutex::held_by_this_thread());
    }

    SECTION("a failed try_lock changes nothing")
    {
        // Another thread holds the lock for the duration of the try_lock
        std::atomic<bool> locked_elsewhere{ false };
        std::atomic<bool> release{ false };
        std::thread other{ [&]
                           {
                               inner.lock();
                               locked_elsewhere = true;
                               while (!release)
                               {
                                   std::this_thread::yield();
                               }
                               inner.unlock();
                           } };
        while (!locked_elsewhere)
        {
            std::this_thread::yield();
        }
        CHECK_FALSE(inner.try_lock());
        CHECK_FALSE(leaf_mutex::held_by_this_thread());
        release = true;
        other.join();
        CHECK(recorded().empty());
    }

    SECTION("locks held on two threads at once are fine")
    {
        // Each thread holds one lock while the other holds its own; Catch2 assertions stay on this thread
        std::atomic<bool> other_holds{ false };
        std::atomic<bool> release{ false };
        std::atomic<bool> held_on_other_thread{ false };
        std::thread other{ [&]
                           {
                               const std::lock_guard other_lock{ inner };
                               held_on_other_thread = leaf_mutex::held_by_this_thread();
                               other_holds = true;
                               while (!release)
                               {
                                   std::this_thread::yield();
                               }
                           } };
        while (!other_holds)
        {
            std::this_thread::yield();
        }
        {
            const std::lock_guard outer_lock{ outer };
            CHECK(leaf_mutex::held_by_this_thread());
        }
        release = true;
        other.join();
        CHECK(held_on_other_thread);
        CHECK(recorded().empty());
    }
}

#endif
