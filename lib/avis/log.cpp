#include "avis/log.h"

#include <format>
#include <mutex>
#include <shared_mutex>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <utility>
#include <vector>

namespace avis::log
{
    namespace
    {
        constexpr std::string_view pattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [t:%N] %v";
        constexpr std::size_t file_maximum_size = 10 * 1024 * 1024;
        constexpr std::size_t file_maximum_count = 5;

        thread_local std::string current_thread_name;

        // "%N": the thread name set with set_thread_name, or the numeric thread id
        class thread_name_flag final : public spdlog::custom_flag_formatter
        {
        public:
            void format(
                const spdlog::details::log_msg& message, const std::tm&, spdlog::memory_buf_t& destination) override
            {
                if (current_thread_name.empty())
                {
                    destination.append(std::to_string(message.thread_id));
                }
                else
                {
                    destination.append(current_thread_name);
                }
            }

            std::unique_ptr<custom_flag_formatter> clone() const override
            {
                return std::make_unique<thread_name_flag>();
            }
        };

        spdlog::level::level_enum to_spdlog(const log_level level) noexcept
        {
            switch (level)
            {
            case log_level::trace:
                return spdlog::level::trace;
            case log_level::debug:
                return spdlog::level::debug;
            case log_level::info:
                return spdlog::level::info;
            case log_level::warn:
                return spdlog::level::warn;
            case log_level::error:
                return spdlog::level::err;
            case log_level::critical:
                return spdlog::level::critical;
            case log_level::off:
                return spdlog::level::off;
            }
            return spdlog::level::off;
        }

        std::unique_ptr<spdlog::formatter> make_formatter()
        {
            auto formatter = std::make_unique<spdlog::pattern_formatter>();
            formatter->add_flag<thread_name_flag>('N').set_pattern(std::string{ pattern });
            return formatter;
        }

        struct logger_entry
        {
            std::shared_ptr<spdlog::logger> logger;
            std::shared_ptr<spdlog::sinks::dist_sink_mt> sinks; // output sinks + attached test sinks
            std::size_t attached_sinks = 0;
        };

        struct registry
        {
            std::shared_mutex mutex;
            std::map<std::string, logger_entry, std::less<>> loggers;
            std::vector<spdlog::sink_ptr> outputs;
            logging_configuration configuration;
            bool initialised = false;

            spdlog::level::level_enum level_for(const std::string_view name, const logger_entry& entry) const
            {
                if (entry.attached_sinks > 0)
                {
                    return spdlog::level::trace;
                }
                if (!initialised)
                {
                    return spdlog::level::off;
                }
                if (const auto level = configuration.levels.find(name); level != configuration.levels.end())
                {
                    return to_spdlog(level->second);
                }
                return to_spdlog(configuration.default_level);
            }

            void apply_level(const std::string_view name, logger_entry& entry) const
            {
                entry.logger->set_level(level_for(name, entry));
            }

            // Requires the exclusive lock
            logger_entry& find_or_create(const std::string_view name)
            {
                if (const auto existing = loggers.find(name); existing != loggers.end())
                {
                    return existing->second;
                }

                auto sinks = std::make_shared<spdlog::sinks::dist_sink_mt>(outputs);
                auto logger = std::make_shared<spdlog::logger>(std::string{ name }, sinks);
                logger->flush_on(spdlog::level::warn);

                auto& entry = loggers.emplace(std::string{ name }, logger_entry{ std::move(logger), std::move(sinks) })
                                  .first->second;
                apply_level(name, entry);
                return entry;
            }
        };

        // Never destroyed, so static destructors in other translation units can still log
        registry& global_registry()
        {
            static auto* instance = new registry{};
            return *instance;
        }
    } // namespace

    std::shared_ptr<spdlog::logger> get(const std::string_view name)
    {
        auto& registry = global_registry();
        {
            std::shared_lock lock{ registry.mutex };
            if (const auto existing = registry.loggers.find(name); existing != registry.loggers.end())
            {
                return existing->second.logger;
            }
        }

        std::unique_lock lock{ registry.mutex };
        return registry.find_or_create(name).logger;
    }

    void set_thread_name(const std::string_view name)
    {
        current_thread_name = name;
    }

    std::string_view thread_name() noexcept
    {
        return current_thread_name;
    }

    namespace detail
    {
        result<void> initialise(const logging_configuration& configuration)
        {
            auto& registry = global_registry();
            std::unique_lock lock{ registry.mutex };

            if (registry.initialised)
            {
                return std::unexpected{ make_error(generic_errc::already_exists, "logging is already initialised") };
            }

            std::vector<spdlog::sink_ptr> outputs;
            try
            {
                if (configuration.debugger_output)
                {
                    outputs.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>(true));
                }
                if (configuration.console)
                {
                    outputs.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
                }
                if (configuration.file)
                {
                    outputs.push_back(
                        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                            configuration.file->string(), file_maximum_size, file_maximum_count));
                }
            }
            catch (const std::exception& exception)
            {
                return std::unexpected{ error{
                    .domain = error_domain::io,
                    .code = 0,
                    .message = std::format("failed to create log sink: {}", exception.what()),
                } };
            }

            for (const auto& output : outputs)
            {
                output->set_formatter(make_formatter());
            }

            // Create the component loggers while there are no outputs yet, so the loop below adds them to
            // every logger exactly once
            for (const auto name : component_loggers)
            {
                registry.find_or_create(name);
            }

            registry.outputs = std::move(outputs);
            registry.configuration = configuration;
            registry.initialised = true;

            for (auto& [name, entry] : registry.loggers)
            {
                for (const auto& output : registry.outputs)
                {
                    entry.sinks->add_sink(output);
                }
                registry.apply_level(name, entry);
            }

            return {};
        }

        void shutdown() noexcept
        {
            auto& registry = global_registry();
            std::unique_lock lock{ registry.mutex };

            if (!registry.initialised)
            {
                return;
            }

            registry.initialised = false;
            for (auto& [name, entry] : registry.loggers)
            {
                try
                {
                    entry.logger->flush();
                }
                catch (...)
                {
                    // Flushing is best effort during shutdown
                }

                for (const auto& output : registry.outputs)
                {
                    entry.sinks->remove_sink(output);
                }
                registry.apply_level(name, entry);
            }

            registry.outputs.clear();
            registry.configuration = {};
        }

        bool initialised() noexcept
        {
            auto& registry = global_registry();
            std::shared_lock lock{ registry.mutex };
            return registry.initialised;
        }

        void attach_sink(const std::string_view logger_name, std::shared_ptr<spdlog::sinks::sink> sink)
        {
            auto& registry = global_registry();
            std::unique_lock lock{ registry.mutex };

            auto& entry = registry.find_or_create(logger_name);
            entry.sinks->add_sink(std::move(sink));
            ++entry.attached_sinks;
            registry.apply_level(logger_name, entry);
        }

        void detach_sink(const std::string_view logger_name, const std::shared_ptr<spdlog::sinks::sink>& sink)
        {
            auto& registry = global_registry();
            std::unique_lock lock{ registry.mutex };

            const auto existing = registry.loggers.find(logger_name);
            if (existing == registry.loggers.end() || existing->second.attached_sinks == 0)
            {
                return;
            }

            auto& entry = existing->second;
            entry.sinks->remove_sink(sink);
            --entry.attached_sinks;
            registry.apply_level(logger_name, entry);
        }
    } // namespace detail
} // namespace avis::log
