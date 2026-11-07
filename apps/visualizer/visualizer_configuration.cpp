#include "visualizer_configuration.h"

#include "rendering/screenshot_capture.h"
#include "rendering/yaml_reading.h"

#include "avis/io/error.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace avis::visualizer
{
    namespace
    {
        namespace yaml_reading = rendering::yaml_reading;
        using yaml_reading::is_given;
        using yaml_reading::scalar;
        using yaml_reading::section;

        constexpr std::string_view default_log_file_name = "visualizer.log";

        // The sections this file reads; any other top-level key is kept as written
        constexpr std::array<std::string_view, 8> known_sections{
            "runtime", "logging", "render", "representation", "screenshots", "lockstep", "network", "ui",
        };

        constexpr std::array<std::pair<std::string_view, log_level>, 7> level_names{ {
            { "trace", log_level::trace },
            { "debug", log_level::debug },
            { "info", log_level::info },
            { "warn", log_level::warn },
            { "error", log_level::error },
            { "critical", log_level::critical },
            { "off", log_level::off },
        } };

        std::string_view level_name(const log_level level)
        {
            for (const auto& [name, value] : level_names)
            {
                if (value == level)
                {
                    return name;
                }
            }
            return "info";
        }

        // "<source>:" in front of an error that has only "<line>:<column>:", so every error names the file
        error with_source(error failure, const std::string_view source_name)
        {
            if (!source_name.empty())
            {
                failure.message = std::format("{}:{}", source_name, failure.message);
            }
            return failure;
        }

        std::string utf8(const std::filesystem::path& path)
        {
            const std::u8string text = path.u8string();
            return std::string{ reinterpret_cast<const char*>(text.data()), text.size() };
        }

        std::filesystem::path from_utf8(const std::string_view text)
        {
            return std::filesystem::path{ std::u8string_view{ reinterpret_cast<const char8_t*>(text.data()),
                                                              text.size() } };
        }

        error io_failure(const std::error_code& code, const std::string_view what, const std::filesystem::path& path)
        {
            io::error_kind kind = io::error_kind::system_error;
            if (code == std::errc::no_such_file_or_directory)
            {
                kind = io::error_kind::not_found;
            }
            else if (code == std::errc::permission_denied)
            {
                kind = io::error_kind::permission_denied;
            }
            else if (code == std::errc::no_space_on_device)
            {
                kind = io::error_kind::disk_full;
            }
            return io::make_error(kind, std::format("{} {}: {}", what, utf8(path), code.message()));
        }

        bool equal(const runtime_configuration& left, const runtime_configuration& right)
        {
            return left.logging.default_level == right.logging.default_level &&
                   left.logging.levels == right.logging.levels && left.logging.file == right.logging.file &&
                   left.logging.console == right.logging.console &&
                   left.logging.debugger_output == right.logging.debugger_output &&
                   left.thread_pool.capacity == right.thread_pool.capacity &&
                   left.thread_pool.join_warning == right.thread_pool.join_warning &&
                   left.io.threads == right.io.threads && left.io.shutdown_timeout == right.io.shutdown_timeout &&
                   left.parallel.workers == right.parallel.workers &&
                   left.application_threads == right.application_threads;
        }

        // A section kept as written: a mapping, or not given
        result<raw_section>
            read_raw_section(const yaml_reading::reader& reader, const YAML::Node& root, const std::string_view name)
        {
            const YAML::Node node = root[std::string{ name }];
            if (!is_given(node))
            {
                return raw_section{};
            }
            if (auto checked = reader.expect_map(node, name); !checked)
            {
                return std::unexpected{ std::move(checked).error() };
            }
            return raw_section{ YAML::Clone(node) };
        }

        result<screenshot_settings> read_screenshots(const yaml_reading::reader& reader, const YAML::Node& root)
        {
            screenshot_settings settings;
            section screenshots{ reader, root["screenshots"], "screenshots" };
            screenshots.read("folder", settings.folder);
            screenshots.read("include_debug_view", settings.include_debug_view);
            section custom = screenshots.child("custom");
            custom.read("width", settings.custom.width);
            custom.check(
                "width",
                settings.custom.width >= 1 && settings.custom.width <= rendering::maximum_screenshot_width,
                std::format("expected 1 to {}, got {}", rendering::maximum_screenshot_width, settings.custom.width));
            custom.read("height", settings.custom.height);
            custom.check(
                "height",
                settings.custom.height >= 1 && settings.custom.height <= rendering::maximum_screenshot_height,
                std::format("expected 1 to {}, got {}", rendering::maximum_screenshot_height, settings.custom.height));
            if (auto failure = custom.take_failure())
            {
                return std::unexpected{ std::move(*failure) };
            }
            return settings;
        }

        result<ui_settings> read_ui(const yaml_reading::reader& reader, const YAML::Node& root)
        {
            ui_settings settings;
            section ui{ reader, root["ui"], "ui" };
            ui.read("last_driver", settings.last_driver);
            const YAML::Node recent = ui.given("recent_files");
            if (!ui.failed() && is_given(recent))
            {
                auto files = reader.text_sequence(recent, ui.path_of("recent_files"));
                if (!files)
                {
                    return std::unexpected{ std::move(files).error() };
                }
                for (const std::string& file : *files)
                {
                    settings.recent_files.push_back(from_utf8(file));
                }
            }
            if (auto failure = ui.take_failure())
            {
                return std::unexpected{ std::move(*failure) };
            }
            return settings;
        }

        YAML::Node runtime_to_yaml(const runtime_configuration& configuration)
        {
            YAML::Node runtime{ YAML::NodeType::Map };
            runtime["io_threads"] = scalar(static_cast<std::uint64_t>(configuration.io.threads));
            runtime["io_shutdown_timeout_ms"] =
                scalar(static_cast<std::uint64_t>(configuration.io.shutdown_timeout.count()));
            runtime["parallel_workers"] = scalar(static_cast<std::uint64_t>(configuration.parallel.workers));
            runtime["thread_pool_capacity"] = scalar(static_cast<std::uint64_t>(configuration.thread_pool.capacity));
            return runtime;
        }

        YAML::Node logging_to_yaml(const logging_configuration& configuration)
        {
            YAML::Node logging{ YAML::NodeType::Map };
            logging["level"] = scalar(level_name(configuration.default_level));
            if (configuration.file)
            {
                logging["file"] = yaml_reading::path_scalar(*configuration.file);
            }
            logging["console"] = scalar(configuration.console);
            logging["debugger_output"] = scalar(configuration.debugger_output);
            if (!configuration.levels.empty())
            {
                YAML::Node levels = yaml_reading::flow_map();
                for (const auto& [name, level] : configuration.levels)
                {
                    levels[name] = scalar(level_name(level));
                }
                logging["levels"] = levels;
            }
            return logging;
        }

        YAML::Node screenshots_to_yaml(const screenshot_settings& settings)
        {
            YAML::Node screenshots{ YAML::NodeType::Map };
            screenshots["folder"] = yaml_reading::path_scalar(settings.folder);
            YAML::Node custom = yaml_reading::flow_map();
            custom["width"] = scalar(settings.custom.width);
            custom["height"] = scalar(settings.custom.height);
            screenshots["custom"] = custom;
            screenshots["include_debug_view"] = scalar(settings.include_debug_view);
            return screenshots;
        }

        YAML::Node ui_to_yaml(const ui_settings& settings)
        {
            YAML::Node ui{ YAML::NodeType::Map };
            ui["last_driver"] = scalar(settings.last_driver);
            YAML::Node recent{ YAML::NodeType::Sequence };
            if (settings.recent_files.empty())
            {
                recent.SetStyle(YAML::EmitterStyle::Flow); // "recent_files: []" on one line
            }
            for (const std::filesystem::path& file : settings.recent_files)
            {
                recent.push_back(yaml_reading::path_scalar(file));
            }
            ui["recent_files"] = recent;
            return ui;
        }
    } // namespace

    bool operator==(const raw_section& left, const raw_section& right)
    {
        const bool left_given = is_given(left.node);
        if (left_given != is_given(right.node))
        {
            return false;
        }
        return !left_given || YAML::Dump(left.node) == YAML::Dump(right.node);
    }

    bool operator==(const visualizer_configuration& left, const visualizer_configuration& right)
    {
        return equal(left.runtime, right.runtime) && left.render == right.render &&
               left.representation == right.representation && left.screenshots == right.screenshots &&
               left.lockstep == right.lockstep && left.network == right.network && left.ui == right.ui &&
               left.unknown_sections == right.unknown_sections;
    }

    result<visualizer_configuration>
        parse_visualizer_configuration(const std::string_view yaml_text, const std::string_view source_name)
    {
        visualizer_configuration configuration;

        // The runtime and logging sections, with the library's own parser (14 §3)
        auto runtime = runtime_configuration::parse(yaml_text);
        if (!runtime)
        {
            return std::unexpected{ with_source(std::move(runtime).error(), source_name) };
        }
        configuration.runtime = std::move(*runtime);

        const yaml_reading::reader reader;
        try
        {
            // runtime_configuration::parse has already rejected text that isn't YAML, or isn't a mapping
            const YAML::Node root = YAML::Load(std::string{ yaml_text });
            if (!root.IsMap())
            {
                return configuration;
            }

            auto render = rendering::parse_render_settings(root["render"]);
            if (!render)
            {
                return std::unexpected{ with_source(std::move(render).error(), source_name) };
            }
            configuration.render = std::move(*render);

            auto representation = representation::parse_representation_settings(root["representation"]);
            if (!representation)
            {
                return std::unexpected{ with_source(std::move(representation).error(), source_name) };
            }
            configuration.representation = std::move(*representation);

            auto screenshots = read_screenshots(reader, root);
            if (!screenshots)
            {
                return std::unexpected{ with_source(std::move(screenshots).error(), source_name) };
            }
            configuration.screenshots = std::move(*screenshots);

            auto lockstep = read_raw_section(reader, root, "lockstep");
            if (!lockstep)
            {
                return std::unexpected{ with_source(std::move(lockstep).error(), source_name) };
            }
            configuration.lockstep = std::move(*lockstep);

            auto network = read_raw_section(reader, root, "network");
            if (!network)
            {
                return std::unexpected{ with_source(std::move(network).error(), source_name) };
            }
            configuration.network = std::move(*network);

            auto ui = read_ui(reader, root);
            if (!ui)
            {
                return std::unexpected{ with_source(std::move(ui).error(), source_name) };
            }
            configuration.ui = std::move(*ui);

            for (const auto& entry : root)
            {
                if (!entry.first.IsScalar())
                {
                    return std::unexpected{
                        with_source(reader.error_at(entry.first, "", "expected a section name"), source_name),
                    };
                }
                if (std::ranges::contains(known_sections, std::string_view{ entry.first.Scalar() }))
                {
                    continue;
                }
                if (!is_given(configuration.unknown_sections.node))
                {
                    configuration.unknown_sections.node = YAML::Node{ YAML::NodeType::Map };
                }
                configuration.unknown_sections.node[entry.first.Scalar()] = YAML::Clone(entry.second);
            }
        }
        catch (const YAML::Exception& exception)
        {
            return std::unexpected{ with_source(reader.error_at(exception.mark, "", exception.msg), source_name) };
        }
        return configuration;
    }

    std::string to_yaml_text(const visualizer_configuration& configuration)
    {
        YAML::Node root{ YAML::NodeType::Map };
        root["runtime"] = runtime_to_yaml(configuration.runtime);
        root["logging"] = logging_to_yaml(configuration.runtime.logging);
        root["render"] = rendering::to_yaml(configuration.render);
        root["representation"] = representation::to_yaml(configuration.representation);
        root["screenshots"] = screenshots_to_yaml(configuration.screenshots);
        if (is_given(configuration.lockstep.node))
        {
            root["lockstep"] = configuration.lockstep.node;
        }
        if (is_given(configuration.network.node))
        {
            root["network"] = configuration.network.node;
        }
        root["ui"] = ui_to_yaml(configuration.ui);
        if (configuration.unknown_sections.node.IsMap())
        {
            for (const auto& entry : configuration.unknown_sections.node)
            {
                root[entry.first.Scalar()] = entry.second;
            }
        }

        YAML::Emitter emitter;
        emitter << YAML::Comment(
                       "Avis Visualizer configuration. The Visualizer rewrites this file: comments aren't kept.")
                << YAML::Newline << root;
        std::string text{ emitter.c_str(), emitter.size() };
        text += '\n';
        return text;
    }

    result<void>
        save_visualizer_configuration(const visualizer_configuration& configuration, const std::filesystem::path& path)
    {
        const std::string text = to_yaml_text(configuration);

        std::error_code code;
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path(), code);
            if (code)
            {
                return std::unexpected{ io_failure(code, "could not create the directory of", path) };
            }
        }

        // Written next to the file and renamed over it, so a failed write never leaves a half-written configuration
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file{ temporary, std::ios::binary | std::ios::trunc };
            if (!file)
            {
                return std::unexpected{ io::make_error(
                    io::error_kind::system_error, std::format("could not open {} for writing", utf8(temporary))) };
            }
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.flush();
            if (!file)
            {
                file.close();
                std::filesystem::remove(temporary, code);
                return std::unexpected{
                    io::make_error(io::error_kind::system_error, std::format("could not write {}", utf8(temporary))),
                };
            }
        }
        std::filesystem::rename(temporary, path, code);
        if (code)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return std::unexpected{ io_failure(code, "could not replace", path) };
        }
        return {};
    }

    result<loaded_configuration> load_visualizer_configuration(const std::filesystem::path& path)
    {
        std::error_code code;
        const bool exists = std::filesystem::exists(path, code);
        if (code)
        {
            return std::unexpected{ io_failure(code, "could not check", path) };
        }

        loaded_configuration loaded;
        if (!exists)
        {
            loaded.created = true;
            if (auto saved = save_visualizer_configuration(loaded.configuration, path); !saved)
            {
                loaded.save_failure = std::move(saved).error();
            }
            return loaded;
        }

        std::ifstream file{ path, std::ios::binary };
        if (!file)
        {
            return std::unexpected{
                io::make_error(io::error_kind::system_error, std::format("could not open {}", utf8(path))),
            };
        }
        const std::string text{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
        if (file.bad())
        {
            return std::unexpected{
                io::make_error(io::error_kind::system_error, std::format("could not read {}", utf8(path))),
            };
        }

        auto configuration = parse_visualizer_configuration(text, utf8(path));
        if (!configuration)
        {
            return std::unexpected{ std::move(configuration).error() };
        }
        loaded.configuration = std::move(*configuration);
        return loaded;
    }

    std::filesystem::path default_configuration_path()
    {
        std::filesystem::path directory;
        wchar_t* app_data = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&app_data, &length, L"APPDATA") == 0 && app_data != nullptr)
        {
            directory = std::filesystem::path{ app_data } / "Avis";
        }
        std::free(app_data);
        return directory / "visualizer.yaml";
    }

    std::filesystem::path resolve_log_file(
        const visualizer_configuration& configuration, const std::filesystem::path& executable_directory)
    {
        const std::optional<std::filesystem::path>& file = configuration.runtime.logging.file;
        if (!file || file->empty())
        {
            return executable_directory / default_log_file_name;
        }
        return file->is_absolute() ? *file : executable_directory / *file;
    }

    result<command_line_options> parse_command_line(const std::span<const std::wstring> arguments)
    {
        constexpr std::wstring_view configuration_option = L"--configuration";
        constexpr std::wstring_view demo_scene_option = L"--demo-scene";

        command_line_options options;
        for (std::size_t index = 0; index < arguments.size(); ++index)
        {
            const std::wstring_view argument = arguments[index];
            std::wstring_view value;
            if (argument == demo_scene_option)
            {
                options.demo_scene = true;
                continue;
            }
            if (argument == configuration_option)
            {
                if (index + 1 == arguments.size())
                {
                    return std::unexpected{
                        make_error(generic_error_kind::invalid_configuration, "--configuration needs a path"),
                    };
                }
                value = arguments[++index];
            }
            else if (
                argument.starts_with(configuration_option) && argument.size() > configuration_option.size() &&
                argument[configuration_option.size()] == L'=')
            {
                value = argument.substr(configuration_option.size() + 1);
            }
            else
            {
                continue; // Qt's, or not an option
            }

            if (value.empty())
            {
                return std::unexpected{
                    make_error(generic_error_kind::invalid_configuration, "--configuration needs a path"),
                };
            }
            options.configuration = std::filesystem::path{ value };
        }
        return options;
    }
} // namespace avis::visualizer
