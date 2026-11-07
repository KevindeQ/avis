#ifndef AVIS_VISUALIZER_VISUALIZER_CONFIGURATION_H
#define AVIS_VISUALIZER_VISUALIZER_CONFIGURATION_H

// The Visualizer's configuration file, visualizer.yaml (docs/design/01-system-architecture.md §7, 12 §8): per machine,
// separate from scenarios. Loaded by main() before the runtime starts, so nothing here uses the runtime's services
// (IO context, logging); files are read and written synchronously. No Qt.

#include "rendering/render_settings.h"
#include "representation/representation_settings.h"

#include "avis/error.h"
#include "avis/runtime_configuration.h"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace avis::visualizer
{
    // `screenshots` (12 §6.3)
    struct screenshot_settings
    {
        struct size
        {
            std::uint32_t width = 3840;
            std::uint32_t height = 2160;

            friend bool operator==(const size&, const size&) = default;
        };

        std::filesystem::path folder; // empty = %USERPROFILE%/Pictures/Avis
        size custom; // the last used custom resolution
        bool include_debug_view = false;

        friend bool operator==(const screenshot_settings&, const screenshot_settings&) = default;
    };

    // `ui`: what the Visualizer remembers between runs (12 §6.2, §8). Window geometry and panel layout are per-user UI
    // state and live in QSettings instead.
    struct ui_settings
    {
        std::string last_driver; // the id of the last connected Driver; empty = none
        std::vector<std::filesystem::path> recent_files; // most recent first

        friend bool operator==(const ui_settings&, const ui_settings&) = default;
    };

    // A section kept as written, until the code that reads it exists (`lockstep` and `network` until Part B), or one
    // the Visualizer doesn't know. Null when the file doesn't have it.
    struct raw_section
    {
        YAML::Node node;

        // Same YAML content (compared as written out)
        friend bool operator==(const raw_section& left, const raw_section& right);
    };

    struct visualizer_configuration
    {
        runtime_configuration runtime; // the `runtime` and `logging` sections (runtime_configuration::parse)
        rendering::render_settings render; // 11 §14
        representation::representation_settings representation; // 12 §9
        screenshot_settings screenshots; // 12 §6.3
        raw_section lockstep; // 02 §6.2, parsed in Part B
        raw_section network; // the Zenoh configuration, parsed in Part B
        ui_settings ui;
        // Top-level sections the Visualizer doesn't know, as a mapping in file order; written back unchanged on save
        raw_section unknown_sections;

        friend bool operator==(const visualizer_configuration& left, const visualizer_configuration& right);
    };

    // Parses visualizer.yaml's text. A missing section, and every missing key, keeps its default; an empty text gives
    // the defaults. Errors are error_domain::parse and read "<source_name>:<line>:<column>: <key path>: <message>",
    // for example "visualizer.yaml:12:16: render.shadows.cascades: expected 1 to 4, got 6". Pure: safe on any thread.
    result<visualizer_configuration>
        parse_visualizer_configuration(std::string_view yaml_text, std::string_view source_name = {});

    // The configuration as visualizer.yaml text, with every known key and the unknown sections after them.
    // parse_visualizer_configuration reads it back to an equal configuration. Comments aren't kept.
    std::string to_yaml_text(const visualizer_configuration& configuration);

    // Writes to_yaml_text to `path`, creating its directory, through a temporary file renamed over it.
    // Errors are error_domain::io.
    result<void>
        save_visualizer_configuration(const visualizer_configuration& configuration, const std::filesystem::path& path);

    struct loaded_configuration
    {
        visualizer_configuration configuration;
        bool created = false; // the file didn't exist: these are the defaults, written out once
        std::optional<error> save_failure; // when created, why the defaults couldn't be written (not fatal)
    };

    // Reads and parses `path`. A missing file gives the defaults and writes them to `path`; failing to write them is
    // reported in save_failure, not as an error. An unreadable file is error_domain::io, a malformed one
    // error_domain::parse with the path as the source name.
    result<loaded_configuration> load_visualizer_configuration(const std::filesystem::path& path);

    // %APPDATA%/Avis/visualizer.yaml, or visualizer.yaml in the working directory without APPDATA
    std::filesystem::path default_configuration_path();

    // The log file: logging.file, relative to the executable's directory, or visualizer.log next to the executable
    // when the configuration doesn't name one
    std::filesystem::path resolve_log_file(
        const visualizer_configuration& configuration, const std::filesystem::path& executable_directory);

    struct command_line_options
    {
        std::optional<std::filesystem::path> configuration; // --configuration <path>
    };

    // Reads the Visualizer's options from the command line's arguments, without the program name. Other arguments are
    // left to Qt and ignored. "--configuration <path>" and "--configuration=<path>" are accepted; the last one wins.
    // A missing path is generic_error_kind::invalid_configuration.
    result<command_line_options> parse_command_line(std::span<const std::wstring> arguments);
} // namespace avis::visualizer

#endif
