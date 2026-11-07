#ifndef VISUALIZER_LOG_LOCATION_H
#define VISUALIZER_LOG_LOCATION_H

#include <filesystem>

// The Visualizer's log file: visualizer.yaml's logging.file, or visualizer.log next to the executable
// (visualizer::resolve_log_file). main() sets the path before it creates the runtime; the startup error dialog names
// it (12 §7).
namespace visualizer_log
{
    // Empty until main() sets it
    inline std::filesystem::path& file_path()
    {
        static std::filesystem::path path;
        return path;
    }
} // namespace visualizer_log

#endif
