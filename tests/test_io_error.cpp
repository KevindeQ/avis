#include "avis/io/error.h"

#include <asio/error.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <windows.h>

namespace
{
    std::error_code win32(const DWORD value)
    {
        return std::error_code{ static_cast<int>(value), std::system_category() };
    }

    bool contains(const std::string_view text, const std::string_view part)
    {
        return text.find(part) != std::string_view::npos;
    }
} // namespace

TEST_CASE("io::make_error classifies Win32 codes", "[io][error]")
{
    struct mapping
    {
        DWORD native;
        avis::io::error_kind expected;
    };

    const auto [native, expected] = GENERATE(
        mapping{ ERROR_FILE_NOT_FOUND, avis::io::error_kind::not_found },
        mapping{ ERROR_PATH_NOT_FOUND, avis::io::error_kind::not_found },
        mapping{ ERROR_ACCESS_DENIED, avis::io::error_kind::permission_denied },
        mapping{ ERROR_SHARING_VIOLATION, avis::io::error_kind::sharing_violation },
        mapping{ ERROR_LOCK_VIOLATION, avis::io::error_kind::sharing_violation },
        mapping{ ERROR_FILE_EXISTS, avis::io::error_kind::already_exists },
        mapping{ ERROR_ALREADY_EXISTS, avis::io::error_kind::already_exists },
        mapping{ ERROR_DISK_FULL, avis::io::error_kind::disk_full },
        mapping{ ERROR_HANDLE_DISK_FULL, avis::io::error_kind::disk_full },
        mapping{ ERROR_INVALID_PARAMETER, avis::io::error_kind::invalid_argument },
        mapping{ ERROR_CRC, avis::io::error_kind::system_error });

    CAPTURE(native);
    const avis::error error = avis::io::make_error(win32(native), "open", "data/frames.avs");

    CHECK(error.domain == avis::error_domain::io);
    CHECK(avis::io::is_error(error, expected));
    CHECK(contains(error.message, std::format("(system error {})", native)));
}

TEST_CASE("io::make_error classifies errno codes from std::filesystem", "[io][error]")
{
    const auto [condition, expected] = GENERATE(
        std::pair{ std::errc::no_such_file_or_directory, avis::io::error_kind::not_found },
        std::pair{ std::errc::permission_denied, avis::io::error_kind::permission_denied },
        std::pair{ std::errc::file_exists, avis::io::error_kind::already_exists },
        std::pair{ std::errc::no_space_on_device, avis::io::error_kind::disk_full },
        std::pair{ std::errc::invalid_argument, avis::io::error_kind::invalid_argument },
        std::pair{ std::errc::file_too_large, avis::io::error_kind::file_too_large },
        std::pair{ std::errc::broken_pipe, avis::io::error_kind::system_error });

    const avis::error error = avis::io::make_error(std::make_error_code(condition), "file_size", "a.bin");

    CHECK(avis::io::is_error(error, expected));
    CHECK(contains(error.message, "(generic error "));
}

TEST_CASE("io::make_error uses the category, not the value alone", "[io][error]")
{
    // asio::error::eof is 2 in asio's misc category, like ERROR_FILE_NOT_FOUND in the system category
    const std::error_code end_of_file = asio::error::make_error_code(asio::error::eof);
    REQUIRE(end_of_file.value() == ERROR_FILE_NOT_FOUND);

    const avis::error error = avis::io::make_error(end_of_file, "read", "a.bin");

    CHECK_FALSE(avis::io::is_error(error, avis::io::error_kind::not_found));
    CHECK(avis::io::is_error(error, avis::io::error_kind::system_error));
    CHECK(contains(error.message, std::format("({} error 2)", end_of_file.category().name())));
}

TEST_CASE("io::make_error formats operation, path and native code", "[io][error]")
{
    SECTION("with a path")
    {
        const avis::error error =
            avis::io::make_error(win32(ERROR_FILE_NOT_FOUND), "open", std::filesystem::path{ "data/scenario.yaml" });

        CHECK(error.message.starts_with("open 'data/scenario.yaml': "));
        CHECK(error.message.ends_with(" (system error 2)"));
        CHECK_FALSE(contains(error.message, "\r"));
        CHECK_FALSE(contains(error.message, "\n"));
    }

    SECTION("without a path")
    {
        const avis::error error = avis::io::make_error(win32(ERROR_ACCESS_DENIED), "asio operation", {});

        CHECK(error.message.starts_with("asio operation: "));
        CHECK(error.message.ends_with(" (system error 5)"));
    }

    SECTION("a non-ASCII path is printed as UTF-8")
    {
        const std::filesystem::path path{ u8"tëst ✓.bin" };
        const avis::error error = avis::io::make_error(win32(ERROR_FILE_NOT_FOUND), "open", path);

        CHECK(contains(error.message, "'t\xC3\xABst \xE2\x9C\x93.bin'"));
    }

    SECTION("std::format prints the error_kind value, not the native code")
    {
        const avis::error error = avis::io::make_error(win32(ERROR_ACCESS_DENIED), "open", "a.bin");

        CHECK(
            std::format("{}", error)
                .starts_with(
                    std::format("[io:{}] ", static_cast<std::int32_t>(avis::io::error_kind::permission_denied))));
    }
}

TEST_CASE("io::make_error with an error_kind", "[io][error]")
{
    const auto expected_line = __LINE__ + 1;
    const avis::error error = avis::io::make_error(avis::io::error_kind::not_open, "read on a closed file");

    CHECK(error.domain == avis::error_domain::io);
    CHECK(error.code == static_cast<std::int32_t>(avis::io::error_kind::not_open));
    CHECK(error.message == "read on a closed file");
    CHECK(std::string_view{ error.location.file_name() }.ends_with("test_io_error.cpp"));
    CHECK(error.location.line() == expected_line);
}

TEST_CASE("io::is_error checks the domain and the code", "[io][error]")
{
    const avis::error io_error = avis::io::make_error(avis::io::error_kind::not_found, "missing");
    CHECK(avis::io::is_error(io_error, avis::io::error_kind::not_found));
    CHECK_FALSE(avis::io::is_error(io_error, avis::io::error_kind::permission_denied));

    // generic_error_kind::invalid_configuration has the same value as io::error_kind::not_found
    const avis::error generic_error = avis::make_error(avis::generic_error_kind::invalid_configuration, "bad");
    REQUIRE(generic_error.code == static_cast<std::int32_t>(avis::io::error_kind::not_found));
    CHECK_FALSE(avis::io::is_error(generic_error, avis::io::error_kind::not_found));
}

TEST_CASE("io::to_string names every error_kind", "[io][error]")
{
    for (std::int32_t value = 0; value <= static_cast<std::int32_t>(avis::io::error_kind::system_error); ++value)
    {
        CAPTURE(value);
        CHECK(avis::io::to_string(static_cast<avis::io::error_kind>(value)) != "unknown");
    }
    CHECK(avis::io::to_string(avis::io::error_kind::sharing_violation) == "sharing_violation");
    CHECK(avis::io::to_string(static_cast<avis::io::error_kind>(1000)) == "unknown");
}
