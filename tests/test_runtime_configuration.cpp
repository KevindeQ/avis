// Tests for the runtime configuration: YAML mapping and resolution

#include "avis/runtime_configuration.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>

namespace
{
    std::size_t expected_default_workers()
    {
        const unsigned int hardware_threads = std::thread::hardware_concurrency();
        return hardware_threads > 2 ? hardware_threads - 2 : 1;
    }
} // namespace

TEST_CASE("the runtime design example parses", "[runtime_configuration]")
{
    const auto configuration = avis::runtime_configuration::parse(R"(
runtime:
  io_threads: 1
  parallel_workers: 0        # 0 = max(1, hardware_concurrency - 2)
  thread_pool_capacity: 0    # 0 = computed
logging:
  level: info
  file: "./logs/driver.log"
)");
    REQUIRE(configuration.has_value());
    CHECK(configuration->io.threads == 1);
    CHECK(configuration->parallel.workers == 0);
    CHECK(configuration->thread_pool.capacity == 0);
    CHECK(configuration->logging.default_level == avis::log_level::info);
    REQUIRE(configuration->logging.file.has_value());
    CHECK(*configuration->logging.file == std::filesystem::path{ "./logs/driver.log" });
}

TEST_CASE("the driver.yaml design example parses", "[runtime_configuration]")
{
    // The other sections are ignored
    const auto configuration = avis::runtime_configuration::parse(R"(
driver_id: driver-0
datasets:
  kitti360: "E:/datasets/kitti360-avis"     # sequence packs, not the raw dataset
plugins:
  search_paths: ["./plugins"]
zenoh:
  mode: peer                                # peer | client
  connect: []                               # e.g. ["tcp/10.0.0.5:7447"] for client mode
  listen: []
  scouting:
    multicast: true                         # answer scouting requests
    autoconnect: []                         # never initiate; the Visualizer scouts and connects
runtime:
  io_threads: 1
  parallel_workers: 0                       # 0 = max(1, hardware_concurrency - 2)
  thread_pool_capacity: 0                   # 0 = computed
logging:
  level: info
  file: "./logs/driver.log"
debug:
  enabled: false
  zenoh_lease: 1h
)");
    REQUIRE(configuration.has_value());
    CHECK(configuration->io.threads == 1);
    CHECK(configuration->logging.default_level == avis::log_level::info);
    CHECK(configuration->logging.file.has_value());
}

TEST_CASE("missing sections and keys keep the defaults", "[runtime_configuration]")
{
    const avis::runtime_configuration defaults;

    for (const std::string text : { "", "zenoh: { mode: peer }", "runtime:\nlogging:\n", "runtime: { io_threads: 3 }" })
    {
        INFO(text);
        const auto configuration = avis::runtime_configuration::parse(text);
        REQUIRE(configuration.has_value());
        CHECK(configuration->parallel.workers == defaults.parallel.workers);
        CHECK(configuration->thread_pool.capacity == defaults.thread_pool.capacity);
        CHECK(configuration->logging.default_level == defaults.logging.default_level);
        CHECK(configuration->logging.debugger_output == defaults.logging.debugger_output);
        CHECK_FALSE(configuration->logging.file.has_value());
    }

    CHECK(avis::runtime_configuration::parse("runtime: { io_threads: 3 }")->io.threads == 3);
}

TEST_CASE("logging options and per-logger levels parse", "[runtime_configuration]")
{
    const auto configuration = avis::runtime_configuration::parse(R"(
logging:
  level: warn
  console: true
  debugger_output: false
  levels: { rhi: debug, io: error }
)");
    REQUIRE(configuration.has_value());
    CHECK(configuration->logging.default_level == avis::log_level::warn);
    CHECK(configuration->logging.console);
    CHECK_FALSE(configuration->logging.debugger_output);
    CHECK(configuration->logging.levels.at("rhi") == avis::log_level::debug);
    CHECK(configuration->logging.levels.at("io") == avis::log_level::error);
}

TEST_CASE("invalid values are parse errors with position and key path", "[runtime_configuration]")
{
    struct invalid_case
    {
        std::string text;
        std::string expected; // part of the message
    };

    for (const auto& [text, expected] : {
             invalid_case{ "runtime:\n  io_threads: many\n", "2:15: runtime.io_threads: expected an unsigned integer" },
             invalid_case{ "runtime: { parallel_workers: -1 }", "runtime.parallel_workers" },
             invalid_case{ "runtime: { thread_pool_capacity: [1, 2] }", "runtime.thread_pool_capacity" },
             invalid_case{ "logging:\n  level: loud\n", "2:10: logging.level: expected one of" },
             invalid_case{ "logging: { levels: { rhi: chatty } }", "logging.levels.rhi" },
             invalid_case{ "logging: { console: maybe }", "logging.console: expected true or false" },
             invalid_case{ "runtime: [1, 2]", "runtime: expected a mapping" },
             invalid_case{ "- just\n- a list\n", "the configuration must be a mapping" },
             invalid_case{ "runtime: { io_threads: 1", "1:" },
         })
    {
        INFO(text);
        const auto configuration = avis::runtime_configuration::parse(text);
        REQUIRE_FALSE(configuration.has_value());
        CHECK(configuration.error().domain == avis::error_domain::parse);
        INFO(configuration.error().message);
        CHECK(configuration.error().message.find(expected) != std::string::npos);
    }
}

TEST_CASE("resolve computes the capacity and the worker count", "[runtime_configuration]")
{
    avis::runtime_configuration configuration;
    configuration.io.threads = 2;
    configuration.application_threads = 3;

    for (const std::size_t workers : { std::size_t{ 0 }, std::size_t{ 5 } })
    {
        configuration.parallel.workers = workers;
        const auto resolved = avis::runtime_configuration::resolve(configuration);
        REQUIRE(resolved.has_value());
        CHECK(resolved->thread_pool.capacity == 2 + 3 + 2); // independent of the parallel workers
        CHECK(resolved->parallel.workers == (workers == 0 ? expected_default_workers() : workers));
    }
}

TEST_CASE("resolve rejects inconsistent configurations", "[runtime_configuration]")
{
    avis::runtime_configuration configuration;
    configuration.io.threads = 2;
    configuration.application_threads = 3;

    configuration.thread_pool.capacity = 4; // below 2 + 3
    auto resolved = avis::runtime_configuration::resolve(configuration);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().code == static_cast<std::int32_t>(avis::generic_errc::invalid_configuration));

    configuration.thread_pool.capacity = 5; // exactly enough is accepted
    CHECK(avis::runtime_configuration::resolve(configuration).has_value());

    configuration.io.threads = 0;
    resolved = avis::runtime_configuration::resolve(configuration);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().code == static_cast<std::int32_t>(avis::generic_errc::invalid_configuration));
}
