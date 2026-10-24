// Tests for avis::io::async_file (docs/tasks/05-io-layer.md §5.5)

#include "helpers/log_capture.h"
#include "helpers/runtime_fixture.h"
#include "helpers/temporary_directory.h"

#include "avis/io/async_file.h"
#include "avis/io/io_context.h"
#include "avis/runtime.h"
#include "avis/sender_of.h"
#include "avis/threading/parallel_helpers.h"
#include "avis/threading/thread_pool.h"

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

using namespace std::chrono_literals;

using avis::io::async_file;
using avis::io::error_kind;
using avis::io::open_flags;
using avis::io::open_mode;
using avis::threading::sync_wait_result;

static_assert(avis::sender_of<avis::io::read_at_sender, std::size_t>);
static_assert(avis::sender_of<avis::io::write_at_sender, std::size_t>);

namespace
{
    avis::runtime_configuration configuration_with_io_threads(const std::size_t threads)
    {
        avis::runtime_configuration configuration = avis::tests::small_runtime_configuration();
        configuration.io.threads = threads;
        return configuration;
    }

    async_file open_or_fail(const std::filesystem::path& path, const open_flags mode)
    {
        auto file = async_file::open(path, mode);
        INFO((file ? std::string{} : file.error().message));
        REQUIRE(file.has_value());
        return std::move(*file);
    }

    // True when nothing else has the file open: a share mode of 0 fails while any other handle exists
    bool no_other_handle_open(const std::filesystem::path& path)
    {
        const HANDLE handle =
            ::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        ::CloseHandle(handle);
        return true;
    }

    // Renames with POSIX semantics (replaces the target even while it is open), via FileRenameInfoEx
    bool posix_rename(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        const HANDLE handle = ::CreateFileW(
            from.c_str(),
            DELETE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        const std::wstring target = to.wstring();
        const std::size_t name_bytes = target.size() * sizeof(wchar_t);
        std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + name_bytes);
        auto* const information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        information->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        information->RootDirectory = nullptr;
        information->FileNameLength = static_cast<DWORD>(name_bytes);
        std::memcpy(information->FileName, target.data(), name_bytes);

        const BOOL renamed =
            ::SetFileInformationByHandle(handle, FileRenameInfoEx, information, static_cast<DWORD>(storage.size()));
        ::CloseHandle(handle);
        return renamed != FALSE;
    }

    bool eventually(const auto& condition, const std::chrono::milliseconds timeout = 2s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }
} // namespace

TEST_CASE("async_file writes and reads back at several offsets", "[io][async_file]")
{
    avis::runtime runtime{ configuration_with_io_threads(GENERATE(std::size_t{ 1 }, std::size_t{ 2 })) };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("round_trip.bin");

    async_file file = open_or_fail(path, open_mode::read | open_mode::write | open_mode::create);
    CHECK(file.is_open());
    CHECK(file.path() == path);

    const std::vector<std::byte> data = avis::tests::pattern(1000, 3);
    for (const std::uint64_t offset : { std::uint64_t{ 0 }, std::uint64_t{ 4096 }, std::uint64_t{ (1 << 20) + 3 } })
    {
        CAPTURE(offset);
        const auto written = sync_wait_result(file.write_at(offset, data));
        REQUIRE(written.has_value());
        CHECK(*written == data.size());
    }

    for (const std::uint64_t offset : { std::uint64_t{ 0 }, std::uint64_t{ 4096 }, std::uint64_t{ (1 << 20) + 3 } })
    {
        CAPTURE(offset);
        std::vector<std::byte> buffer(data.size());
        const auto read = sync_wait_result(file.read_at(offset, buffer));
        REQUIRE(read.has_value());
        CHECK(*read == data.size());
        CHECK(buffer == data);
    }

    const auto size = file.size();
    REQUIRE(size.has_value());
    CHECK(*size == (1 << 20) + 3 + data.size());

    REQUIRE(file.resize(10).has_value());
    CHECK(*file.size() == 10);
    CHECK(file.flush().has_value());
}

TEST_CASE("async_file reads at and beyond the end of the file", "[io][async_file]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("short.bin");
    const std::vector<std::byte> data = avis::tests::pattern(100);
    avis::tests::write_file(path, data);

    const async_file file = open_or_fail(path, open_mode::read);
    std::vector<std::byte> buffer(50);

    SECTION("exactly at the end")
    {
        const auto read = sync_wait_result(file.read_at(100, buffer));
        REQUIRE(read.has_value());
        CHECK(*read == 0);
    }

    SECTION("straddling the end")
    {
        const auto read = sync_wait_result(file.read_at(80, buffer));
        REQUIRE(read.has_value());
        CHECK(*read == 20);
        CHECK(std::equal(buffer.begin(), buffer.begin() + 20, data.begin() + 80));
    }

    SECTION("past the end")
    {
        const auto read = sync_wait_result(file.read_at(1000, buffer));
        REQUIRE(read.has_value());
        CHECK(*read == 0);
    }

    SECTION("an empty buffer")
    {
        const auto read = sync_wait_result(file.read_at(0, std::span<std::byte>{}));
        REQUIRE(read.has_value());
        CHECK(*read == 0);
    }
}

TEST_CASE("async_file reads 64 MiB in one read_at", "[io][async_file]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("large.bin");
    constexpr std::size_t size = 64u << 20;
    const std::vector<std::byte> data = avis::tests::pattern(size, 11);
    avis::tests::write_file(path, data);

    const async_file file = open_or_fail(path, open_mode::read);
    std::vector<std::byte> buffer(size);
    const auto read = sync_wait_result(file.read_at(0, buffer));
    REQUIRE(read.has_value());
    CHECK(*read == size);
    CHECK(buffer == data);
}

TEST_CASE("async_file::open reports errors", "[io][async_file]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto existing = directory.file("existing.bin");
    avis::tests::write_file(existing, avis::tests::pattern(10));

    SECTION("a missing file")
    {
        const auto file = async_file::open(directory.file("missing.bin"), open_mode::read);
        REQUIRE_FALSE(file.has_value());
        CHECK(avis::io::is_error(file.error(), error_kind::not_found));
        CHECK(file.error().message.starts_with("open '"));
    }

    SECTION("exclusive creation of an existing file")
    {
        const auto file = async_file::open(existing, open_mode::write | open_mode::create | open_mode::exclusive);
        REQUIRE_FALSE(file.has_value());
        CHECK(avis::io::is_error(file.error(), error_kind::already_exists));
    }

    SECTION("a read-only file opened for writing")
    {
        REQUIRE(::SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_READONLY));
        const auto file = async_file::open(existing, open_mode::write);
        ::SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_NORMAL);
        REQUIRE_FALSE(file.has_value());
        CHECK(avis::io::is_error(file.error(), error_kind::permission_denied));
    }

    SECTION("invalid flag combinations")
    {
        for (const open_flags mode : { open_flags{ avis::empty },
                                       open_mode::read | open_mode::create,
                                       open_mode::read | open_mode::truncate,
                                       open_mode::write | open_mode::exclusive })
        {
            CAPTURE(mode.underlying_value());
            const auto file = async_file::open(existing, mode);
            REQUIRE_FALSE(file.has_value());
            CHECK(avis::io::is_error(file.error(), error_kind::invalid_argument));
        }
    }

    SECTION("a path with non-ASCII characters")
    {
        const auto path = directory.file(reinterpret_cast<const char*>(u8"tëst ✓.bin"));
        const async_file file = open_or_fail(path, open_mode::write | open_mode::create);
        const std::vector<std::byte> data = avis::tests::pattern(16);
        CHECK(sync_wait_result(file.write_at(0, data)).has_value());
        CHECK(std::filesystem::exists(path));
    }
}

TEST_CASE("a file open for reading can be renamed over", "[io][async_file]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto target = directory.file("target.bin");
    const auto replacement = directory.file("replacement.bin");
    const std::vector<std::byte> old_data = avis::tests::pattern(256, 1);
    const std::vector<std::byte> new_data = avis::tests::pattern(256, 2);
    avis::tests::write_file(target, old_data);
    avis::tests::write_file(replacement, new_data);

    const async_file file = open_or_fail(target, open_mode::read);

    // A classic rename (MoveFileExW, std::filesystem::rename) can't replace a file with open handles, whatever their
    // share mode. A POSIX-semantics rename can, as long as every open handle shares delete, which read-only
    // async_files do.
    REQUIRE(posix_rename(replacement, target));

    // The open handle still sees the old file; the path now has the new one
    std::vector<std::byte> buffer(old_data.size());
    REQUIRE(sync_wait_result(file.read_at(0, buffer)).has_value());
    CHECK(buffer == old_data);
    CHECK(avis::tests::read_file(target) == new_data);
}

TEST_CASE("a stop token cancels its own read only", "[io][async_file]")
{
    avis::runtime runtime{ configuration_with_io_threads(2) };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("cancel.bin");
    constexpr std::size_t size = 16u << 20;
    const std::vector<std::byte> data = avis::tests::pattern(size, 5);
    avis::tests::write_file(path, data);
    const async_file file = open_or_fail(path, open_mode::read);

    SECTION("stop requested before start")
    {
        std::vector<std::byte> buffer(size);
        stdexec::inplace_stop_source source;
        source.request_stop();
        const auto result = sync_wait_result(
            stdexec::write_env(file.read_at(0, buffer), stdexec::prop{ stdexec::get_stop_token, source.get_token() }));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == static_cast<std::int32_t>(avis::generic_error_kind::cancelled));
    }

    SECTION("stop requested while reading, next to a read that isn't stopped")
    {
        int stopped = 0;
        for (int iteration = 0; iteration < 20; ++iteration)
        {
            std::vector<std::byte> cancelled_buffer(size);
            std::vector<std::byte> other_buffer(size);
            stdexec::inplace_stop_source source;

            std::optional<avis::result<std::size_t>> other;
            std::thread other_reader{ [&] { other = sync_wait_result(file.read_at(0, other_buffer)); } };
            std::thread stopper{ [&source] { source.request_stop(); } };

            const auto cancelled = sync_wait_result(
                stdexec::write_env(
                    file.read_at(0, cancelled_buffer), stdexec::prop{ stdexec::get_stop_token, source.get_token() }));
            stopper.join();
            other_reader.join();

            // Either stopped, or it finished before the stop arrived; never an error or a partial count
            if (cancelled.has_value())
            {
                CHECK(*cancelled == size);
            }
            else
            {
                CHECK(cancelled.error().code == static_cast<std::int32_t>(avis::generic_error_kind::cancelled));
                ++stopped;
            }

            REQUIRE(other.has_value());
            REQUIRE(other->has_value());
            CHECK(**other == size);
            CHECK(other_buffer == data);
        }
        INFO("reads stopped: " << stopped << " of 20");
        SUCCEED();
    }
}

TEST_CASE("many threads read one async_file concurrently", "[io][async_file]")
{
    avis::runtime runtime{ configuration_with_io_threads(GENERATE(std::size_t{ 1 }, std::size_t{ 2 })) };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("shared.bin");
    constexpr std::size_t size = 1u << 20;
    constexpr std::size_t read_size = 4096;
    const std::vector<std::byte> data = avis::tests::pattern(size, 9);
    avis::tests::write_file(path, data);
    const async_file file = open_or_fail(path, open_mode::read);

    std::atomic<int> wrong = 0;
    std::vector<std::jthread> readers;
    for (unsigned int thread = 0; thread < 16; ++thread)
    {
        readers.emplace_back(
            [&file, &data, &wrong, thread]
            {
                std::mt19937 random{ thread };
                std::uniform_int_distribution<std::size_t> offsets{ 0, size - read_size };
                std::vector<std::byte> buffer(read_size);
                for (int read = 0; read < 200; ++read)
                {
                    const std::size_t offset = offsets(random);
                    const auto result = sync_wait_result(file.read_at(offset, buffer));
                    if (!result || *result != read_size ||
                        !std::equal(buffer.begin(), buffer.end(), data.begin() + static_cast<std::ptrdiff_t>(offset)))
                    {
                        wrong.fetch_add(1);
                    }
                }
            });
    }
    readers.clear(); // joins

    CHECK(wrong == 0);
}

TEST_CASE("close while other threads start reads created before it", "[io][async_file]")
{
    avis::runtime runtime{ configuration_with_io_threads(2) };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("close_race.bin");
    constexpr std::size_t size = 1u << 20;
    avis::tests::write_file(path, avis::tests::pattern(size));

    for (int iteration = 0; iteration < 20; ++iteration)
    {
        constexpr int reader_count = 8;
        std::vector<std::vector<std::byte>> buffers(reader_count, std::vector<std::byte>(size));
        std::atomic<int> errors = 0;
        std::atomic<int> completed = 0;

        {
            async_file file = open_or_fail(path, open_mode::read);
            REQUIRE_FALSE(no_other_handle_open(path));

            std::vector<std::optional<avis::io::read_at_sender>> senders;
            for (int reader = 0; reader < reader_count; ++reader)
            {
                senders.emplace_back(file.read_at(0, buffers[reader]));
            }

            std::barrier start{ reader_count + 1 };
            std::vector<std::jthread> readers;
            for (int reader = 0; reader < reader_count; ++reader)
            {
                readers.emplace_back(
                    [&, reader]
                    {
                        auto sender = std::move(*senders[reader]);
                        senders[reader].reset();
                        start.arrive_and_wait();
                        const auto result = sync_wait_result(std::move(sender));
                        if (!result &&
                            result.error().code != static_cast<std::int32_t>(avis::generic_error_kind::cancelled))
                        {
                            errors.fetch_add(1);
                        }
                        completed.fetch_add(1);
                    });
            }

            start.arrive_and_wait();
            file.close();
            CHECK_FALSE(file.is_open());
            readers.clear(); // joins
        }

        CHECK(errors == 0);
        CHECK(completed == reader_count);
        // Every reference is gone: the handle closes (possibly on an IO thread, right after the last completion)
        CHECK(eventually([&] { return no_other_handle_open(path); }));
    }
}

TEST_CASE("destroying an async_file with reads in flight", "[io][async_file]")
{
    avis::runtime runtime{ configuration_with_io_threads(2) };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("destroy.bin");
    constexpr std::size_t size = 8u << 20;
    avis::tests::write_file(path, avis::tests::pattern(size));

    constexpr int read_count = 16;
    std::vector<std::vector<std::byte>> buffers(read_count, std::vector<std::byte>(size));
    std::atomic<int> values = 0;
    std::atomic<int> stopped = 0;
    std::atomic<int> errors = 0;

    {
        async_file file = open_or_fail(path, open_mode::read);
        for (int read = 0; read < read_count; ++read)
        {
            exec::start_detached(
                file.read_at(0, buffers[read]) |
                stdexec::then([&values](std::size_t) noexcept { values.fetch_add(1); }) |
                stdexec::upon_stopped([&stopped]() noexcept { stopped.fetch_add(1); }) |
                stdexec::upon_error([&errors](avis::error) noexcept { errors.fetch_add(1); }));
        }
    } // destroyed with the reads in flight

    CHECK(eventually([&] { return values + stopped + errors == read_count; }));
    CHECK(errors == 0);
    CHECK(eventually([&] { return no_other_handle_open(path); }));
}

TEST_CASE("operations on a closed async_file fail with not_open", "[io][async_file]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("closed.bin");
    avis::tests::write_file(path, avis::tests::pattern(16));
    std::vector<std::byte> buffer(16);

    SECTION("default-constructed")
    {
        const async_file file;
        CHECK_FALSE(file.is_open());
        CHECK(file.path().empty());
        const auto read = sync_wait_result(file.read_at(0, buffer));
        REQUIRE_FALSE(read.has_value());
        CHECK(avis::io::is_error(read.error(), error_kind::not_open));
        CHECK(avis::io::is_error(file.size().error(), error_kind::not_open));
        CHECK(avis::io::is_error(file.flush().error(), error_kind::not_open));
    }

    SECTION("moved-from")
    {
        async_file file = open_or_fail(path, open_mode::read);
        const async_file moved = std::move(file);
        CHECK(moved.is_open());
        const auto read = sync_wait_result(file.read_at(0, buffer)); // NOLINT(bugprone-use-after-move)
        REQUIRE_FALSE(read.has_value());
        CHECK(avis::io::is_error(read.error(), error_kind::not_open));
        CHECK(sync_wait_result(moved.read_at(0, buffer)).has_value());
    }

    SECTION("closed")
    {
        async_file file = open_or_fail(path, open_mode::read);
        file.close();
        file.close(); // idempotent
        const auto write = sync_wait_result(file.write_at(0, buffer));
        REQUIRE_FALSE(write.has_value());
        CHECK(avis::io::is_error(write.error(), error_kind::not_open));
    }
}

TEST_CASE("runtime shutdown with reads in flight", "[io][async_file]")
{
    avis::tests::temporary_directory directory;
    const auto path = directory.file("shutdown.bin");
    constexpr std::size_t size = 8u << 20;
    avis::tests::write_file(path, avis::tests::pattern(size));

    constexpr int read_count = 32;
    std::vector<std::vector<std::byte>> buffers(read_count, std::vector<std::byte>(size));
    std::atomic<int> completed = 0;
    avis::tests::log_capture capture{ "io" };

    {
        avis::runtime runtime{ configuration_with_io_threads(2) };
        async_file file = open_or_fail(path, open_mode::read);
        for (int read = 0; read < read_count; ++read)
        {
            exec::start_detached(
                file.read_at(0, buffers[read]) | stdexec::then([](std::size_t) noexcept {}) |
                stdexec::upon_error([](avis::error) noexcept {}) |
                stdexec::then([&completed]() noexcept { completed.fetch_add(1); }) |
                stdexec::upon_stopped([&completed]() noexcept { completed.fetch_add(1); }));
        }
        file.close(); // the async_file must not outlive the runtime's IO context; the reads keep its state alive
    } // the runtime cancels what is still running and waits for it

    CHECK(completed == read_count);
    CHECK_FALSE(capture.contains("shutdown timeout"));
}

TEST_CASE("io_context::request_stop cancels reads of an open async_file", "[io][async_file]")
{
    avis::tests::temporary_directory directory;
    const auto path = directory.file("request_stop.bin");
    constexpr std::size_t size = 8u << 20;
    avis::tests::write_file(path, avis::tests::pattern(size));

    constexpr int read_count = 32;
    std::vector<std::vector<std::byte>> buffers(read_count, std::vector<std::byte>(size));
    std::atomic<int> values = 0;
    std::atomic<int> stopped = 0;
    avis::tests::log_capture capture{ "io" };

    avis::threading::thread_pool pool{ { .capacity = 2 } };
    avis::io::io_context io{ pool, { .threads = 2, .shutdown_timeout = 5s } };
    {
        auto opened = async_file::open(io, path, open_mode::read);
        REQUIRE(opened.has_value());
        async_file file = std::move(*opened);

        for (int read = 0; read < read_count; ++read)
        {
            exec::start_detached(
                file.read_at(0, buffers[read]) |
                stdexec::then([&values](std::size_t) noexcept { values.fetch_add(1); }) |
                stdexec::upon_stopped([&stopped]() noexcept { stopped.fetch_add(1); }) |
                stdexec::upon_error([](avis::error) noexcept {}));
        }

        // The file is still open: only the registry can cancel what is still running
        io.request_stop();
        CHECK(eventually([&] { return values + stopped == read_count; }));
        CHECK_FALSE(capture.contains("shutdown timeout"));

        // New reads are refused once the context is stopping
        std::vector<std::byte> late(16);
        const auto result = sync_wait_result(file.read_at(0, late));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == static_cast<std::int32_t>(avis::generic_error_kind::cancelled));
    }
}
