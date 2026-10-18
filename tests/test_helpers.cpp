#include "helpers/thread_count.h"

#include <catch2/catch_test_macros.hpp>

#include <latch>
#include <thread>

TEST_CASE("current_process_thread_count sees threads start and exit", "[helpers]")
{
    const std::size_t before = avis::tests::current_process_thread_count();
    CHECK(before >= 1);

    {
        std::latch started{ 1 };
        std::latch release{ 1 };
        std::jthread thread{ [&]
                             {
                                 started.count_down();
                                 release.wait();
                             } };

        started.wait();
        CHECK(avis::tests::current_process_thread_count() == before + 1);
        release.count_down();
    }

    CHECK(avis::tests::current_process_thread_count() == before);
}
