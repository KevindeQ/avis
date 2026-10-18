#ifndef AVIS_LOG_H
#define AVIS_LOG_H

#include "avis/error.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <spdlog/logger.h>
#include <spdlog/sinks/sink.h>
#include <string>
#include <string_view>

namespace avis
{
    enum class log_level : std::uint8_t
    {
        trace,
        debug,
        info,
        warn,
        error,
        critical,
        off
    };

    // Owned by runtime_configuration (14 §3); defined here because avis::log is initialised from it
    struct logging_configuration
    {
        log_level default_level = log_level::info;
        std::map<std::string, log_level, std::less<>> levels; // per logger, e.g. {"rhi", debug}
        std::optional<std::filesystem::path> file; // rotating file sink
        bool console = false;
        bool debugger_output = true; // OutputDebugString sink
    };
} // namespace avis

namespace avis::log
{
    // Component loggers registered at initialisation (13 §7). Other names, such as "plugin.<name>",
    // are created on first use.
    inline constexpr std::array<std::string_view, 10> component_loggers{ "core",  "io",     "network", "rhi",
                                                                         "graph", "render", "scene",   "simulation",
                                                                         "ui",    "driver" };

    // Returns the named logger. Safe to call at any time and from any thread: before initialisation and
    // after shutdown the logger has no output sinks and drops every message (14 §5).
    std::shared_ptr<spdlog::logger> get(std::string_view name);

    // Name shown in the "[t:...]" field of the log pattern for the calling thread. The thread pool sets it
    // for its threads; threads without a name show their numeric id.
    void set_thread_name(std::string_view name);
    std::string_view thread_name() noexcept;

    namespace detail
    {
        // Used by avis::runtime only. Fails if logging is already initialised or a sink can't be created
        // (for example an invalid log file path).
        result<void> initialise(const logging_configuration& configuration);
        void shutdown() noexcept;
        bool initialised() noexcept;

        // Test hooks: route a logger's messages to an extra sink as well. While at least one extra sink is
        // attached, the logger logs every level.
        void attach_sink(std::string_view logger_name, std::shared_ptr<spdlog::sinks::sink> sink);
        void detach_sink(std::string_view logger_name, const std::shared_ptr<spdlog::sinks::sink>& sink);
    } // namespace detail
} // namespace avis::log

#endif
