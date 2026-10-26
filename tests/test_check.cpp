// AVIS_CHECK and the assertion handler (lib/avis/detail/check.h, docs/tasks/06-rhi.md §6.1)

#include "helpers/log_capture.h"

#include "avis/detail/check.h"

#include <catch2/catch_test_macros.hpp>

#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct recorded_assertion
    {
        std::string message;
        std::source_location location;
    };

    std::vector<recorded_assertion>& recorded_assertions()
    {
        static std::vector<recorded_assertion> assertions;
        return assertions;
    }

    void recording_handler(const std::string_view message, const std::source_location location) noexcept
    {
        recorded_assertions().push_back({ std::string{ message }, location });
    }

    void other_handler(std::string_view, std::source_location) noexcept {}

    // Installs the recording handler for one test case and restores the previous handler afterwards
    class recording_handler_scope
    {
    public:
        recording_handler_scope() : previous_{ avis::detail::set_assertion_handler(&recording_handler) }
        {
            recorded_assertions().clear();
        }

        ~recording_handler_scope()
        {
            avis::detail::set_assertion_handler(previous_);
            recorded_assertions().clear();
        }

        recording_handler_scope(const recording_handler_scope&) = delete;
        recording_handler_scope& operator=(const recording_handler_scope&) = delete;

    private:
        avis::detail::assertion_handler previous_;
    };

    constexpr std::string_view logger_name = "test.check";
} // namespace

TEST_CASE("a failing AVIS_CHECK yields false, logs and calls the assertion handler", "[check]")
{
    const recording_handler_scope handler;
    const avis::tests::log_capture capture{ std::string{ logger_name } };

    const auto expected_line = std::source_location::current().line() + 1;
    const bool passed = AVIS_CHECK(logger_name, 1 + 1 == 3, "arithmetic is broken");

    CHECK_FALSE(passed);

#ifdef NDEBUG
    CHECK(recorded_assertions().empty());
    CHECK(capture.contains("[error] check failed: arithmetic is broken"));
#else
    REQUIRE(recorded_assertions().size() == 1);
    const auto& assertion = recorded_assertions().front();
    CHECK(assertion.message == "arithmetic is broken");
    CHECK(std::string_view{ assertion.location.file_name() }.ends_with("test_check.cpp"));
    CHECK(assertion.location.line() == expected_line);
    CHECK(capture.contains("[critical] check failed: arithmetic is broken"));
#endif
    CHECK(capture.contains("test_check.cpp:" + std::to_string(expected_line)));
}

TEST_CASE("a passing AVIS_CHECK yields true and logs nothing", "[check]")
{
    const recording_handler_scope handler;
    const avis::tests::log_capture capture{ std::string{ logger_name } };

    CHECK(AVIS_CHECK(logger_name, 1 + 1 == 2, "never logged"));

    CHECK(recorded_assertions().empty());
    CHECK(capture.lines().empty());
}

TEST_CASE("AVIS_CHECK evaluates its condition exactly once", "[check]")
{
    const recording_handler_scope handler;
    const avis::tests::log_capture capture{ std::string{ logger_name } };

    int evaluations = 0;
    CHECK(AVIS_CHECK(logger_name, ++evaluations == 1, "first evaluation"));
    CHECK(evaluations == 1);

    CHECK_FALSE(AVIS_CHECK(logger_name, ++evaluations == 1, "second evaluation"));
    CHECK(evaluations == 2);
}

TEST_CASE("set_assertion_handler returns the previous handler", "[check]")
{
    const auto original = avis::detail::set_assertion_handler(&recording_handler);
    CHECK(original != nullptr);

    CHECK(avis::detail::set_assertion_handler(&other_handler) == &recording_handler);
    CHECK(avis::detail::set_assertion_handler(original) == &other_handler);
}
