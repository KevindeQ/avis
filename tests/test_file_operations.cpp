// Tests for avis::io::read_file, write_file and file_size (docs/tasks/05-io-layer.md §5.6)

#include "helpers/runtime_fixture.h"
#include "helpers/temporary_directory.h"

#include "avis/io/async_file.h"
#include "avis/io/file_operations.h"
#include "avis/io/io_context.h"
#include "avis/io/mapped_file.h"
#include "avis/runtime.h"
#include "avis/sender_of.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using avis::io::error_kind;
using avis::io::read_file;
using avis::io::write_file;
using avis::io::write_policy;
using avis::threading::sync_wait_result;

static_assert(avis::sender_of<avis::io::read_file_sender, std::vector<std::byte>>);
static_assert(avis::sender_of<avis::io::write_file_sender>);

namespace
{
    avis::runtime_configuration configuration_with_io_threads(const std::size_t threads)
    {
        avis::runtime_configuration configuration = avis::tests::small_runtime_configuration();
        configuration.io.threads = threads;
        return configuration;
    }

    std::string policy_name(const write_policy policy)
    {
        switch (policy)
        {
        case write_policy::atomic_replace:
            return "atomic_replace";
        case write_policy::truncate:
            return "truncate";
        case write_policy::create_new:
            return "create_new";
        }
        return "unknown";
    }

    // Files in the directory whose name ends in .tmp (atomic_replace's temporary files)
    std::size_t temporary_files_in(const std::filesystem::path& directory)
    {
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator{ directory })
        {
            if (entry.path().extension() == ".tmp")
            {
                ++count;
            }
        }
        return count;
    }

    // Turns the atomic_replace failure injection on for its lifetime
    class injected_atomic_replace_failure
    {
    public:
        injected_atomic_replace_failure() noexcept
        {
            avis::io::detail::inject_atomic_replace_failure(true);
        }

        ~injected_atomic_replace_failure()
        {
            avis::io::detail::inject_atomic_replace_failure(false);
        }

        injected_atomic_replace_failure(const injected_atomic_replace_failure&) = delete;
        injected_atomic_replace_failure& operator=(const injected_atomic_replace_failure&) = delete;
    };

    bool is_cancelled(const avis::error& error)
    {
        return error.domain == avis::error_domain::generic &&
               error.code == static_cast<std::int32_t>(avis::generic_error_kind::cancelled);
    }

    // 07 §4.7: a trivial parser that throws on an empty input
    struct scenario
    {
        std::size_t size = 0;
    };

    scenario parse_scenario(const std::vector<std::byte>& bytes)
    {
        if (bytes.empty())
        {
            throw std::runtime_error{ "empty scenario" };
        }
        return scenario{ bytes.size() };
    }

    auto load_scenario_async(const std::filesystem::path& path)
    {
        return avis::io::read_file(path) | stdexec::continues_on(stdexec::get_parallel_scheduler()) |
               stdexec::then([](std::vector<std::byte> bytes) { return parse_scenario(bytes); }) |
               avis::threading::into_internal_error();
    }
} // namespace

TEST_CASE("write_file then read_file round trip", "[io][file_operations]")
{
    avis::runtime runtime{ configuration_with_io_threads(GENERATE(std::size_t{ 1 }, std::size_t{ 2 })) };
    avis::tests::temporary_directory directory;

    const write_policy policy =
        GENERATE(write_policy::atomic_replace, write_policy::truncate, write_policy::create_new);
    const std::size_t size = GENERATE(std::size_t{ 0 }, std::size_t{ 1 }, std::size_t{ 8 } << 20);
    INFO("policy " << policy_name(policy) << ", " << size << " bytes");

    const auto path = directory.file("round_trip.bin");
    const std::vector<std::byte> data = avis::tests::pattern(size, 7);

    const auto written = sync_wait_result(write_file(path, data, policy));
    INFO((written ? std::string{} : written.error().message));
    REQUIRE(written.has_value());

    const auto read = sync_wait_result(read_file(path));
    REQUIRE(read.has_value());
    CHECK(*read == data);
    CHECK(avis::io::file_size(path) == size);
    CHECK(temporary_files_in(directory.path()) == 0);

    // Writing again replaces the contents (create_new refuses, see below)
    if (policy != write_policy::create_new)
    {
        const std::vector<std::byte> shorter = avis::tests::pattern(size / 2, 8);
        REQUIRE(sync_wait_result(write_file(path, shorter, policy)).has_value());
        CHECK(avis::tests::read_file(path) == shorter);
    }
}

TEST_CASE("create_new refuses an existing file", "[io][file_operations]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("existing.bin");
    const std::vector<std::byte> old_data = avis::tests::pattern(100, 1);
    avis::tests::write_file(path, old_data);

    const auto written = sync_wait_result(write_file(path, avis::tests::pattern(50, 2), write_policy::create_new));
    REQUIRE_FALSE(written.has_value());
    CHECK(avis::io::is_error(written.error(), error_kind::already_exists));
    CHECK(avis::tests::read_file(path) == old_data);
}

TEST_CASE("a failing atomic_replace leaves the target and no temporary file", "[io][file_operations]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("target.bin");
    const std::vector<std::byte> old_data = avis::tests::pattern(1000, 1);
    avis::tests::write_file(path, old_data);

    {
        const injected_atomic_replace_failure failure;
        const auto written = sync_wait_result(write_file(path, avis::tests::pattern(2000, 2)));
        REQUIRE_FALSE(written.has_value());
        CHECK(avis::io::is_error(written.error(), error_kind::system_error));
        CHECK(written.error().message.find("injected failure") != std::string::npos);
    }

    CHECK(avis::tests::read_file(path) == old_data);
    CHECK(temporary_files_in(directory.path()) == 0);
}

TEST_CASE("atomic_replace replaces a file that is open for reading and mapped", "[io][file_operations]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("target.bin");
    const std::vector<std::byte> old_data = avis::tests::pattern(4096, 1);
    const std::vector<std::byte> new_data = avis::tests::pattern(3000, 2);
    avis::tests::write_file(path, old_data);

    auto opened = avis::io::async_file::open(path, avis::io::open_mode::read);
    REQUIRE(opened.has_value());
    const avis::io::async_file file = std::move(*opened);
    auto mapped = avis::io::mapped_file::open(path);
    REQUIRE(mapped.has_value());

    const auto written = sync_wait_result(write_file(path, new_data));
    INFO((written ? std::string{} : written.error().message));
    REQUIRE(written.has_value());

    // The open handle and the mapping still see the old file; the path has the new one
    std::vector<std::byte> buffer(old_data.size());
    REQUIRE(sync_wait_result(file.read_at(0, buffer)) == old_data.size());
    CHECK(buffer == old_data);
    CHECK(std::ranges::equal(mapped->bytes(), old_data));

    const auto read = sync_wait_result(read_file(path));
    REQUIRE(read.has_value());
    CHECK(*read == new_data);
    CHECK(temporary_files_in(directory.path()) == 0);
}

TEST_CASE("concurrent atomic_replace writes of one path", "[io][file_operations]")
{
    avis::runtime runtime{ configuration_with_io_threads(2) };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("contended.bin");
    const std::vector<std::byte> first = avis::tests::pattern(1 << 20, 1);
    const std::vector<std::byte> second = avis::tests::pattern(1 << 19, 2);

    for (int iteration = 0; iteration < 20; ++iteration)
    {
        const auto written = sync_wait_result(stdexec::when_all(write_file(path, first), write_file(path, second)));
        INFO((written ? std::string{} : written.error().message));
        REQUIRE(written.has_value());

        const std::vector<std::byte> contents = avis::tests::read_file(path);
        CHECK((contents == first || contents == second));
        CHECK(temporary_files_in(directory.path()) == 0);
    }
}

TEST_CASE("where write_file completes", "[io][file_operations]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const std::vector<std::byte> data = avis::tests::pattern(4096, 3);

    SECTION("atomic_replace on a parallel worker")
    {
        const auto on_worker = sync_wait_result(
            write_file(directory.file("atomic.bin"), data) |
            stdexec::then([] { return avis::threading::detail::in_parallel_work(); }));
        REQUIRE(on_worker.has_value());
        CHECK(*on_worker);
    }

    SECTION("truncate on an IO thread")
    {
        const auto on_io_thread = sync_wait_result(
            write_file(directory.file("truncate.bin"), data, write_policy::truncate) |
            stdexec::then([] { return avis::io::io_context::current() != nullptr; }));
        REQUIRE(on_io_thread.has_value());
        CHECK(*on_io_thread);
    }

    SECTION("read_file on an IO thread")
    {
        avis::tests::write_file(directory.file("read.bin"), data);
        const auto on_io_thread = sync_wait_result(
            read_file(directory.file("read.bin")) |
            stdexec::then([](std::vector<std::byte>) { return avis::io::io_context::current() != nullptr; }));
        REQUIRE(on_io_thread.has_value());
        CHECK(*on_io_thread);
    }
}

TEST_CASE("the owning write_file overload keeps its data alive", "[io][file_operations]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("owned.bin");
    const std::vector<std::byte> expected = avis::tests::pattern(1 << 20, 4);
    const write_policy policy = GENERATE(write_policy::atomic_replace, write_policy::truncate);
    INFO("policy " << policy_name(policy));

    SECTION("moved in")
    {
        std::optional<avis::io::write_file_sender> sender;
        {
            std::vector<std::byte> data = expected;
            sender.emplace(write_file(path, std::move(data), policy));
        } // the caller's vector is gone; ASan would report a read of it
        REQUIRE(sync_wait_result(std::move(*sender)).has_value());
        CHECK(avis::tests::read_file(path) == expected);
    }

    SECTION("copied from an lvalue")
    {
        std::optional<avis::io::write_file_sender> sender;
        {
            const std::vector<std::byte> data = expected;
            sender.emplace(write_file(path, data, policy)); // picks the owning overload, which copies
        }
        REQUIRE(sync_wait_result(std::move(*sender)).has_value());
        CHECK(avis::tests::read_file(path) == expected);
    }
}

TEST_CASE("missing files", "[io][file_operations]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;
    const auto missing = directory.file("missing.bin");

    const auto read = sync_wait_result(read_file(missing));
    REQUIRE_FALSE(read.has_value());
    CHECK(avis::io::is_error(read.error(), error_kind::not_found));

    const auto size = avis::io::file_size(missing);
    REQUIRE_FALSE(size.has_value());
    CHECK(avis::io::is_error(size.error(), error_kind::not_found));
    CHECK(size.error().message.find("file_size") != std::string::npos);

    const auto existing = directory.file("existing.bin");
    avis::tests::write_file(existing, avis::tests::pattern(12345));
    CHECK(avis::io::file_size(existing) == 12345u);

    // atomic_replace into a missing directory fails at open, with nothing to clean up
    const auto written =
        sync_wait_result(write_file(directory.file("no/such/directory.bin"), avis::tests::pattern(10)));
    REQUIRE_FALSE(written.has_value());
    CHECK(avis::io::is_error(written.error(), error_kind::not_found));
}

TEST_CASE("stopping a write_file", "[io][file_operations]")
{
    avis::runtime runtime{ configuration_with_io_threads(2) };
    avis::tests::temporary_directory directory;
    const auto path = directory.file("stopped.bin");
    const std::vector<std::byte> old_data = avis::tests::pattern(100, 1);
    const std::vector<std::byte> new_data = avis::tests::pattern(16u << 20, 2);
    avis::tests::write_file(path, old_data);

    SECTION("stop requested before start")
    {
        stdexec::inplace_stop_source source;
        source.request_stop();
        const auto written = sync_wait_result(
            stdexec::write_env(
                write_file(path, new_data), stdexec::prop{ stdexec::get_stop_token, source.get_token() }));
        REQUIRE_FALSE(written.has_value());
        CHECK(is_cancelled(written.error()));
        CHECK(avis::tests::read_file(path) == old_data);
        CHECK(temporary_files_in(directory.path()) == 0);
    }

    SECTION("stop requested while writing: replaced completely or not at all")
    {
        int stopped = 0;
        for (int iteration = 0; iteration < 20; ++iteration)
        {
            avis::tests::write_file(path, old_data);
            stdexec::inplace_stop_source source;
            std::thread stopper{ [&source] { source.request_stop(); } };
            const auto written = sync_wait_result(
                stdexec::write_env(
                    write_file(path, new_data), stdexec::prop{ stdexec::get_stop_token, source.get_token() }));
            stopper.join();

            if (written.has_value())
            {
                CHECK(avis::tests::read_file(path) == new_data);
            }
            else
            {
                CHECK(is_cancelled(written.error()));
                CHECK(avis::tests::read_file(path) == old_data);
                ++stopped;
            }
            CHECK(temporary_files_in(directory.path()) == 0);
        }
        INFO("writes stopped: " << stopped << " of 20");
        SUCCEED();
    }
}

TEST_CASE("the 07 load_scenario_async example", "[io][file_operations]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    avis::tests::temporary_directory directory;

    const auto path = directory.file("scenario.yaml");
    avis::tests::write_file(path, avis::tests::pattern(321));
    const avis::result<scenario> loaded = sync_wait_result(load_scenario_async(path));
    REQUIRE(loaded.has_value());
    CHECK(loaded->size == 321);

    const auto empty = directory.file("empty.yaml");
    avis::tests::write_file(empty, {});
    const avis::result<scenario> failed = sync_wait_result(load_scenario_async(empty));
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message.find("empty scenario") != std::string::npos);
}
