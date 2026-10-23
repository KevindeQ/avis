#ifndef AVIS_TESTS_HELPERS_RUNTIME_FIXTURE_H
#define AVIS_TESTS_HELPERS_RUNTIME_FIXTURE_H

#include "avis/runtime.h"

#include <cstddef>

namespace avis::tests
{
    // A small runtime for tests: 1 IO thread, 2 parallel workers unless asked otherwise, room for 1 application thread,
    // no log output
    inline avis::runtime_configuration small_runtime_configuration(const std::size_t parallel_workers = 2)
    {
        avis::runtime_configuration configuration;
        configuration.io.threads = 1;
        configuration.parallel.workers = parallel_workers;
        configuration.application_threads = 1;
        configuration.logging.console = false;
        configuration.logging.debugger_output = false;
        configuration.logging.file.reset();
        return configuration;
    }

    // Catch2 fixture (TEST_CASE_METHOD) that keeps a small runtime alive for the test case
    class runtime_fixture
    {
    protected:
        runtime_fixture() : runtime_{ small_runtime_configuration() } {}

        avis::runtime runtime_;
    };

    // Makes the runtime's static accessors throw instead of asserting in debug builds while alive
    class throwing_runtime_accessors
    {
    public:
        throwing_runtime_accessors() noexcept
        {
            avis::detail::make_runtime_accessors_throw(true);
        }

        ~throwing_runtime_accessors()
        {
            avis::detail::make_runtime_accessors_throw(false);
        }

        throwing_runtime_accessors(const throwing_runtime_accessors&) = delete;
        throwing_runtime_accessors& operator=(const throwing_runtime_accessors&) = delete;
    };
} // namespace avis::tests

#endif
