#ifndef VISUALIZER_LOG_LOCATION_H
#define VISUALIZER_LOG_LOCATION_H

#include <filesystem>
#include <string_view>

// The Visualizer's log file: visualizer.log next to the executable, until 12 reads visualizer.yaml's logging section.
// main() sets the path before it creates the runtime; the startup error dialog names it (12 §7).
namespace visualizer_log
{
    inline constexpr std::string_view file_name = "visualizer.log";

    // Empty until main() sets it
    inline std::filesystem::path& file_path()
    {
        static std::filesystem::path path;
        return path;
    }
} // namespace visualizer_log

#endif
