#include "avis/error.h"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <format>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace
{
    bool ends_with_file(const std::source_location& location, const std::string_view file)
    {
        return std::string_view{ location.file_name() }.ends_with(file);
    }
} // namespace

TEST_CASE("error formats as [domain:code] message (file:line)", "[error]")
{
    const avis::error error{
        .domain = avis::error_domain::io,
        .code = 5,
        .message = "read 'scenario.yaml': access denied",
    };

    const std::string expected =
        std::format("[io:5] read 'scenario.yaml': access denied (test_error.cpp:{})", error.location.line());
    CHECK(std::format("{}", error) == expected);
}

TEST_CASE("error captures the source location where it is created", "[error]")
{
    SECTION("aggregate initialisation")
    {
        const auto expected_line = __LINE__ + 1;
        const avis::error error{ .message = "here" };

        CHECK(ends_with_file(error.location, "test_error.cpp"));
        CHECK(error.location.line() == expected_line);
    }

    SECTION("make_error with a generic code")
    {
        const auto expected_line = __LINE__ + 1;
        const avis::error error = avis::make_error(avis::generic_error_kind::pool_exhausted, "no free slot");

        CHECK(error.domain == avis::error_domain::generic);
        CHECK(error.code == static_cast<std::int32_t>(avis::generic_error_kind::pool_exhausted));
        CHECK(error.message == "no free slot");
        CHECK(ends_with_file(error.location, "test_error.cpp"));
        CHECK(error.location.line() == expected_line);
    }
}

TEST_CASE("make_error converts exceptions", "[error]")
{
    SECTION("avis::runtime_error keeps its error")
    {
        const avis::error original{ .domain = avis::error_domain::rhi, .code = 42, .message = "device removed" };

        try
        {
            throw avis::runtime_error{ original };
        }
        catch (...)
        {
            const avis::error error = avis::make_error(std::current_exception());
            CHECK(error.domain == avis::error_domain::rhi);
            CHECK(error.code == 42);
            CHECK(error.message == "device removed");
            CHECK(error.location.line() == original.location.line());
        }
    }

    SECTION("std::system_error keeps its code")
    {
        try
        {
            throw std::system_error{ std::make_error_code(std::errc::permission_denied), "open" };
        }
        catch (...)
        {
            const avis::error error = avis::make_error(std::current_exception());
            CHECK(error.domain == avis::error_domain::generic);
            CHECK(error.code == static_cast<std::int32_t>(std::errc::permission_denied));
            CHECK(std::string_view{ error.message }.starts_with("open"));
        }
    }

    SECTION("std::exception keeps what()")
    {
        try
        {
            throw std::logic_error{ "invariant violated" };
        }
        catch (...)
        {
            const auto expected_line = __LINE__ + 1;
            const avis::error error = avis::make_error(std::current_exception());
            CHECK(error.code == static_cast<std::int32_t>(avis::generic_error_kind::unhandled_exception));
            CHECK(error.message == "invariant violated");
            CHECK(error.location.line() == expected_line);
        }
    }

    SECTION("any other exception")
    {
        try
        {
            throw 7;
        }
        catch (...)
        {
            const avis::error error = avis::make_error(std::current_exception());
            CHECK(error.code == static_cast<std::int32_t>(avis::generic_error_kind::unhandled_exception));
            CHECK(error.message == "unknown exception");
        }
    }

    SECTION("no exception")
    {
        const avis::error error = avis::make_error(std::exception_ptr{});
        CHECK(error.code == static_cast<std::int32_t>(avis::generic_error_kind::logic_error));
    }
}

TEST_CASE("runtime_error exposes its error and message", "[error]")
{
    const avis::runtime_error exception{ avis::make_error(
        avis::generic_error_kind::invalid_configuration, "capacity too small") };

    CHECK(std::string_view{ exception.what() } == "capacity too small");
    CHECK(exception.error().code == static_cast<std::int32_t>(avis::generic_error_kind::invalid_configuration));
}

TEST_CASE("result carries a value or an error", "[error]")
{
    const avis::result<int> value = 3;
    const avis::result<int> failure =
        std::unexpected{ avis::make_error(avis::generic_error_kind::cancelled, "stopped") };

    CHECK(value.has_value());
    CHECK(*value == 3);
    REQUIRE_FALSE(failure.has_value());
    CHECK(failure.error().code == static_cast<std::int32_t>(avis::generic_error_kind::cancelled));
}

TEST_CASE("domains and generic codes have names", "[error]")
{
    CHECK(avis::to_string(avis::error_domain::generic) == "generic");
    CHECK(avis::to_string(avis::error_domain::protocol) == "protocol");
    CHECK(avis::to_string(avis::error_domain::graphics) == "graphics");
    CHECK(avis::to_string(avis::generic_error_kind::pool_stopping) == "pool_stopping");
}
