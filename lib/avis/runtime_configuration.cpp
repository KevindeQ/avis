#include "avis/runtime_configuration.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <yaml-cpp/yaml.h>

namespace avis
{
    namespace
    {
        struct level_name
        {
            std::string_view name;
            log_level level;
        };

        constexpr std::array<level_name, 7> level_names{ { { "trace", log_level::trace },
                                                           { "debug", log_level::debug },
                                                           { "info", log_level::info },
                                                           { "warn", log_level::warn },
                                                           { "error", log_level::error },
                                                           { "critical", log_level::critical },
                                                           { "off", log_level::off } } };

        error parse_error(const YAML::Mark& mark, const std::string_view path, const std::string_view message)
        {
            // yaml-cpp counts from 0 and uses -1 when it has no position
            const int line = mark.line >= 0 ? mark.line + 1 : 0;
            const int column = mark.column >= 0 ? mark.column + 1 : 0;
            return error{
                .domain = error_domain::parse,
                .code = 0,
                .message = path.empty() ? std::format("{}:{}: {}", line, column, message) :
                                          std::format("{}:{}: {}: {}", line, column, path, message),
            };
        }

        // Reads typed values from one section into the configuration, leaving a target unchanged when its key is
        // missing, and names the key path in errors
        class section_reader
        {
        public:
            section_reader(const YAML::Node& section, std::string section_name) :
                section_{ section },
                section_name_{ std::move(section_name) }
            {}

            std::optional<error> read_count(const std::string_view key, std::size_t& target) const
            {
                const YAML::Node node = section_[std::string{ key }];
                if (!node.IsDefined() || node.IsNull())
                {
                    return std::nullopt;
                }

                try
                {
                    if (node.IsScalar())
                    {
                        const auto value = node.as<std::int64_t>();
                        if (value >= 0)
                        {
                            target = static_cast<std::size_t>(value);
                            return std::nullopt;
                        }
                    }
                }
                catch (const YAML::Exception&)
                {
                    // Reported below
                }
                return parse_error(node.Mark(), path(key), "expected an unsigned integer");
            }

            std::optional<error> read_flag(const std::string_view key, bool& target) const
            {
                const YAML::Node node = section_[std::string{ key }];
                if (!node.IsDefined() || node.IsNull())
                {
                    return std::nullopt;
                }

                try
                {
                    if (node.IsScalar())
                    {
                        target = node.as<bool>();
                        return std::nullopt;
                    }
                }
                catch (const YAML::Exception&)
                {
                    // Reported below
                }
                return parse_error(node.Mark(), path(key), "expected true or false");
            }

            std::optional<error> read_text(const std::string_view key, std::optional<std::string>& target) const
            {
                const YAML::Node node = section_[std::string{ key }];
                if (!node.IsDefined() || node.IsNull())
                {
                    return std::nullopt;
                }
                if (!node.IsScalar())
                {
                    return parse_error(node.Mark(), path(key), "expected a string");
                }
                target = node.Scalar();
                return std::nullopt;
            }

            static std::optional<error> to_level(const YAML::Node& node, const std::string& key_path, log_level& target)
            {
                if (node.IsScalar())
                {
                    for (const auto& [name, level] : level_names)
                    {
                        if (node.Scalar() == name)
                        {
                            target = level;
                            return std::nullopt;
                        }
                    }
                }
                return parse_error(
                    node.Mark(), key_path, "expected one of trace, debug, info, warn, error, critical, off");
            }

            std::optional<error> read_level(const std::string_view key, log_level& target) const
            {
                const YAML::Node node = section_[std::string{ key }];
                if (!node.IsDefined() || node.IsNull())
                {
                    return std::nullopt;
                }
                return to_level(node, path(key), target);
            }

            std::string path(const std::string_view key) const
            {
                return std::format("{}.{}", section_name_, key);
            }

            const YAML::Node& node() const noexcept
            {
                return section_;
            }

        private:
            YAML::Node section_;
            std::string section_name_;
        };

        // The named top-level section, or nullopt when it's missing or empty; an error when it isn't a map
        result<std::optional<section_reader>> section(const YAML::Node& root, const std::string_view name)
        {
            const YAML::Node node = root[std::string{ name }];
            if (!node.IsDefined() || node.IsNull())
            {
                return std::optional<section_reader>{};
            }
            if (!node.IsMap())
            {
                return std::unexpected{ parse_error(node.Mark(), name, "expected a mapping") };
            }
            return std::optional<section_reader>{ section_reader{ node, std::string{ name } } };
        }

        std::optional<error> read_runtime(const section_reader& runtime, runtime_configuration& configuration)
        {
            if (auto failure = runtime.read_count("io_threads", configuration.io.threads))
            {
                return failure;
            }
            if (auto failure = runtime.read_count("parallel_workers", configuration.parallel.workers))
            {
                return failure;
            }
            return runtime.read_count("thread_pool_capacity", configuration.thread_pool.capacity);
        }

        std::optional<error> read_logging(const section_reader& logging, logging_configuration& configuration)
        {
            if (auto failure = logging.read_level("level", configuration.default_level))
            {
                return failure;
            }

            std::optional<std::string> file;
            if (auto failure = logging.read_text("file", file))
            {
                return failure;
            }
            if (file)
            {
                configuration.file = std::filesystem::path{ *file };
            }

            if (auto failure = logging.read_flag("console", configuration.console))
            {
                return failure;
            }
            if (auto failure = logging.read_flag("debugger_output", configuration.debugger_output))
            {
                return failure;
            }

            // levels: { rhi: debug, io: warn }
            const YAML::Node levels = logging.node()["levels"];
            if (!levels.IsDefined() || levels.IsNull())
            {
                return std::nullopt;
            }
            if (!levels.IsMap())
            {
                return parse_error(
                    levels.Mark(), logging.path("levels"), "expected a mapping of logger names to levels");
            }
            for (const auto& entry : levels)
            {
                const std::string name = entry.first.Scalar();
                log_level level = log_level::info;
                if (auto failure = section_reader::to_level(entry.second, logging.path("levels") + "." + name, level))
                {
                    return failure;
                }
                configuration.levels.insert_or_assign(name, level);
            }
            return std::nullopt;
        }
    } // namespace

    result<runtime_configuration> runtime_configuration::parse(const std::string_view yaml_text)
    {
        YAML::Node root;
        try
        {
            root = YAML::Load(std::string{ yaml_text });
        }
        catch (const YAML::ParserException& exception)
        {
            return std::unexpected{ parse_error(exception.mark, "", exception.msg) };
        }

        runtime_configuration configuration;
        if (root.IsNull())
        {
            return configuration;
        }
        if (!root.IsMap())
        {
            return std::unexpected{ parse_error(root.Mark(), "", "the configuration must be a mapping") };
        }

        try
        {
            const auto runtime = section(root, "runtime");
            if (!runtime)
            {
                return std::unexpected{ runtime.error() };
            }
            if (*runtime)
            {
                if (auto failure = read_runtime(**runtime, configuration))
                {
                    return std::unexpected{ std::move(*failure) };
                }
            }

            const auto logging = section(root, "logging");
            if (!logging)
            {
                return std::unexpected{ logging.error() };
            }
            if (*logging)
            {
                if (auto failure = read_logging(**logging, configuration.logging))
                {
                    return std::unexpected{ std::move(*failure) };
                }
            }
        }
        catch (const YAML::Exception& exception)
        {
            return std::unexpected{ parse_error(exception.mark, "", exception.msg) };
        }

        return configuration;
    }

    result<runtime_configuration> runtime_configuration::resolve(runtime_configuration configuration)
    {
        if (configuration.io.threads == 0)
        {
            return std::unexpected{ make_error(
                generic_error_kind::invalid_configuration, "runtime configuration: io.threads must be at least 1") };
        }

        configuration.parallel.workers = resolve_worker_count(configuration.parallel.workers);

        const std::size_t required = configuration.io.threads + configuration.application_threads;
        if (configuration.thread_pool.capacity == 0)
        {
            configuration.thread_pool.capacity = required + 2; // headroom
        }
        else if (configuration.thread_pool.capacity < required)
        {
            return std::unexpected{ make_error(
                generic_error_kind::invalid_configuration,
                std::format(
                    "runtime configuration: thread_pool.capacity {} is below the {} threads needed "
                    "(io.threads {} + application_threads {})",
                    configuration.thread_pool.capacity,
                    required,
                    configuration.io.threads,
                    configuration.application_threads)) };
        }

        return configuration;
    }

    // The number of parallel workers to start for a configured value: 0 becomes max(1, hardware_concurrency - 2),
    // and the result fits the uint32_t that exec::static_thread_pool takes
    std::size_t runtime_configuration::resolve_worker_count(std::size_t workers) noexcept
    {
        if (workers == 0)
        {
            const unsigned int hardware_threads = std::thread::hardware_concurrency();
            workers = hardware_threads > 2 ? hardware_threads - 2 : 1;
        }
        return std::min<std::size_t>(workers, std::numeric_limits<std::uint32_t>::max());
    }
} // namespace avis
