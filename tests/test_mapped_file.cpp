// Tests for avis::io::mapped_file (docs/tasks/05-io-layer.md §5.7)

#include "helpers/runtime_fixture.h"
#include "helpers/temporary_directory.h"

#include "avis/io/file_operations.h"
#include "avis/io/mapped_file.h"
#include "avis/runtime.h"
#include "avis/threading/parallel_helpers.h"

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

using avis::io::error_kind;
using avis::io::mapped_file;

namespace
{
    mapped_file open_or_fail(const std::filesystem::path& path)
    {
        auto mapped = mapped_file::open(path);
        INFO((mapped ? std::string{} : mapped.error().message));
        REQUIRE(mapped.has_value());
        return std::move(*mapped);
    }

    bool same_bytes(const std::span<const std::byte> bytes, const std::vector<std::byte>& expected)
    {
        return std::ranges::equal(bytes, expected);
    }

    DWORD handle_count()
    {
        DWORD count = 0;
        REQUIRE(::GetProcessHandleCount(::GetCurrentProcess(), &count));
        return count;
    }
} // namespace

TEST_CASE("mapped_file maps a file's contents", "[io][mapped_file]")
{
    avis::tests::temporary_directory directory;

    SECTION("1 MiB file")
    {
        const auto path = directory.file("data.bin");
        const std::vector<std::byte> data = avis::tests::pattern(1 << 20, 9);
        avis::tests::write_file(path, data);

        const mapped_file mapped = open_or_fail(path);
        CHECK(mapped.is_open());
        CHECK(mapped.path() == path);
        CHECK(mapped.size() == data.size());
        CHECK(same_bytes(mapped.bytes(), data));
    }

    SECTION("empty file")
    {
        const auto path = directory.file("empty.bin");
        avis::tests::write_file(path, {});

        const mapped_file mapped = open_or_fail(path);
        CHECK(mapped.is_open());
        CHECK(mapped.size() == 0);
        CHECK(mapped.bytes().empty());
    }

    SECTION("missing file")
    {
        const auto mapped = mapped_file::open(directory.file("missing.bin"));
        REQUIRE_FALSE(mapped.has_value());
        CHECK(avis::io::is_error(mapped.error(), error_kind::not_found));
    }

    SECTION("default-constructed")
    {
        const mapped_file closed;
        CHECK_FALSE(closed.is_open());
        CHECK(closed.size() == 0);
        CHECK(closed.bytes().empty());
        CHECK(closed.path().empty());
    }
}

TEST_CASE("mapped_file moves", "[io][mapped_file]")
{
    avis::tests::temporary_directory directory;
    const auto first_path = directory.file("first.bin");
    const auto second_path = directory.file("second.bin");
    const std::vector<std::byte> first_data = avis::tests::pattern(10000, 1);
    const std::vector<std::byte> second_data = avis::tests::pattern(20000, 2);
    avis::tests::write_file(first_path, first_data);
    avis::tests::write_file(second_path, second_data);

    SECTION("construction")
    {
        mapped_file source = open_or_fail(first_path);
        const std::byte* const view = source.bytes().data();

        const mapped_file target{ std::move(source) };
        CHECK_FALSE(source.is_open());
        CHECK(source.bytes().empty());
        CHECK(source.path().empty());
        CHECK(target.bytes().data() == view);
        CHECK(target.path() == first_path);
        CHECK(same_bytes(target.bytes(), first_data));
    }

    SECTION("assignment unmaps the target's mapping")
    {
        mapped_file source = open_or_fail(first_path);
        mapped_file target = open_or_fail(second_path);

        target = std::move(source);
        CHECK_FALSE(source.is_open());
        CHECK(same_bytes(target.bytes(), first_data));

        // second.bin is no longer open: a delete succeeds without delete sharing tricks
        CHECK(::DeleteFileW(second_path.c_str()));
    }
}

TEST_CASE("mapped_file leaks no handles", "[io][mapped_file]")
{
    avis::tests::temporary_directory directory;
    const auto path = directory.file("cycles.bin");
    const auto empty_path = directory.file("empty.bin");
    avis::tests::write_file(path, avis::tests::pattern(4096));
    avis::tests::write_file(empty_path, {});

    // Warm up anything the first open allocates once
    {
        const mapped_file warm_up = open_or_fail(path);
    }

    const DWORD before = handle_count();
    for (int cycle = 0; cycle < 1000; ++cycle)
    {
        mapped_file mapped = open_or_fail(cycle % 2 == 0 ? path : empty_path);
        mapped_file moved = std::move(mapped);
        mapped = std::move(moved);
        const auto missing = mapped_file::open(directory.file("missing.bin"));
        CHECK_FALSE(missing.has_value());
    }
    const DWORD after = handle_count();

    // A little slack for handles other threads of the test process may open meanwhile
    CHECK(after <= before + 2);
}

TEST_CASE("an atomic_replace of a mapped file keeps the mapping's contents", "[io][mapped_file]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("mapped.bin");
    const std::vector<std::byte> old_data = avis::tests::pattern(1 << 16, 1);
    const std::vector<std::byte> new_data = avis::tests::pattern(1 << 15, 2);
    avis::tests::write_file(path, old_data);

    const mapped_file mapped = open_or_fail(path);

    const auto written = avis::threading::sync_wait_result(avis::io::write_file(path, new_data));
    INFO((written ? std::string{} : written.error().message));
    REQUIRE(written.has_value());

    CHECK(same_bytes(mapped.bytes(), old_data));
    CHECK(avis::tests::read_file(path) == new_data);
}
