#include "helpers/log_capture.h"

#include "avis/log.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // Initialises logging for one test case and shuts it down again, so test cases don't affect each other
    class logging_session
    {
    public:
        explicit logging_session(const avis::logging_configuration& configuration)
        {
            const auto initialised = avis::log::detail::initialise(configuration);
            REQUIRE(initialised.has_value());
        }

        ~logging_session()
        {
            avis::log::detail::shutdown();
        }

        logging_session(const logging_session&) = delete;
        logging_session& operator=(const logging_session&) = delete;
    };

    avis::logging_configuration silent_configuration()
    {
        avis::logging_configuration configuration;
        configuration.debugger_output = false;
        configuration.console = false;
        return configuration;
    }

    // A unique, empty directory under the system temporary directory, removed afterwards
    class temporary_directory
    {
    public:
        temporary_directory()
        {
            const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() / ("avis-test-log-" + std::to_string(tick));
            std::filesystem::create_directories(path_);
        }

        ~temporary_directory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream stream{ path, std::ios::binary };
        return std::string{ std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };
    }
} // namespace

TEST_CASE("loggers are usable before logging is initialised", "[log]")
{
    REQUIRE_FALSE(avis::log::detail::initialised());

    const auto logger = avis::log::get("core");
    REQUIRE(logger != nullptr);
    CHECK_FALSE(logger->should_log(spdlog::level::critical));

    // Dropped, not crashing
    logger->critical("dropped {}", 1);
}

TEST_CASE("initialise applies the default and per-logger levels", "[log]")
{
    auto configuration = silent_configuration();
    configuration.default_level = avis::log_level::warn;
    configuration.levels["rhi"] = avis::log_level::debug;

    {
        logging_session session{ configuration };

        CHECK(avis::log::detail::initialised());
        CHECK(avis::log::get("core")->level() == spdlog::level::warn);
        CHECK(avis::log::get("rhi")->level() == spdlog::level::debug);

        // Created on first use, with the default level
        CHECK(avis::log::get("plugin.example")->level() == spdlog::level::warn);

        const auto second = avis::log::detail::initialise(configuration);
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == static_cast<std::int32_t>(avis::generic_errc::already_exists));
    }

    CHECK_FALSE(avis::log::detail::initialised());
    CHECK(avis::log::get("rhi")->level() == spdlog::level::off);
}

TEST_CASE("log_capture records a logger's messages", "[log]")
{
    SECTION("without initialisation")
    {
        avis::tests::log_capture capture{ "core" };
        avis::log::get("core")->info("value {}", 42);

        CHECK(capture.lines() == std::vector<std::string>{ "[info] value 42" });
    }

    SECTION("while initialised, independent of the configured level")
    {
        auto configuration = silent_configuration();
        configuration.default_level = avis::log_level::error;
        logging_session session{ configuration };

        avis::tests::log_capture capture{ "io" };
        avis::log::get("io")->debug("debug message");
        avis::log::get("core")->error("other logger");

        CHECK(capture.contains("[debug] debug message"));
        CHECK_FALSE(capture.contains("other logger"));
    }

    // Detaching the capture restores the logger's level
    CHECK(avis::log::get("core")->level() == spdlog::level::off);
    CHECK(avis::log::get("io")->level() == spdlog::level::off);
}

TEST_CASE("the file sink writes the configured pattern with the thread name", "[log]")
{
    temporary_directory directory;
    const auto file = directory.path() / "logs" / "test.log";

    auto configuration = silent_configuration();
    configuration.file = file;
    {
        logging_session session{ configuration };

        avis::log::set_thread_name("avis-test-main");
        avis::log::get("core")->info("hello {}", "file");
        avis::log::set_thread_name("");
    }

    const std::string contents = read_file(file);
    CHECK(contents.contains("] [core] [info] [t:avis-test-main] hello file"));
}

TEST_CASE("initialise fails for an unusable log file path", "[log]")
{
    temporary_directory directory;
    const auto blocker = directory.path() / "blocker";
    std::ofstream{ blocker } << "not a directory";

    auto configuration = silent_configuration();
    configuration.file = blocker / "test.log";

    const auto initialised = avis::log::detail::initialise(configuration);
    REQUIRE_FALSE(initialised.has_value());
    CHECK(initialised.error().domain == avis::error_domain::io);
    CHECK_FALSE(avis::log::detail::initialised());
}

TEST_CASE("get and logging are safe while logging is initialised and shut down", "[log]")
{
    std::atomic<bool> stop = false;
    std::vector<std::jthread> threads;
    for (int index = 0; index < 4; ++index)
    {
        threads.emplace_back(
            [&stop, index]
            {
                const std::string name = "plugin.concurrent-" + std::to_string(index);
                while (!stop.load())
                {
                    avis::log::get(name)->info("message from {}", index);
                    avis::log::get("core")->warn("shared logger");
                }
            });
    }

    for (int iteration = 0; iteration < 50; ++iteration)
    {
        logging_session session{ silent_configuration() };
    }

    stop = true;
    threads.clear();
    CHECK_FALSE(avis::log::detail::initialised());
}
