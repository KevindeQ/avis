#include "rendering/renderer_shaders.h"

#include "rendering/shader_manifest_parser.h"

#include "avis/io/error.h"

#include <format>
#include <string>

namespace avis::visualizer::rendering
{
    std::filesystem::path
        resolve_shader_directory(const render_settings& settings, const std::filesystem::path& executable_directory)
    {
        if (settings.shader_directory.empty())
        {
            return (executable_directory / default_shader_subdirectory).lexically_normal();
        }
        if (settings.shader_directory.is_absolute())
        {
            return settings.shader_directory.lexically_normal();
        }
        return (executable_directory / settings.shader_directory).lexically_normal();
    }

    namespace detail
    {
        error manifest_read_error(error failure, const std::filesystem::path& directory)
        {
            const std::u8string directory_text = directory.generic_u8string();
            const std::string_view directory_name{ reinterpret_cast<const char*>(directory_text.data()),
                                                   directory_text.size() };
            if (io::is_error(failure, io::error_kind::not_found))
            {
                failure.message = std::format(
                    "{} not found in {}; was the Visualizer built and deployed with its shaders? ({})",
                    shader_manifest_file_name,
                    directory_name,
                    failure.message);
            }
            else
            {
                failure.message =
                    std::format("couldn't read the shader manifest in {}: {}", directory_name, failure.message);
            }
            return failure;
        }

        graphics::shader_manifest
            parse_manifest_or_throw(const std::span<const std::byte> bytes, const std::filesystem::path& manifest_path)
        {
            const std::string_view text{ reinterpret_cast<const char*>(bytes.data()), bytes.size() };
            const std::u8string source_name = manifest_path.generic_u8string();
            auto manifest = parse_shader_manifest(
                text, std::string_view{ reinterpret_cast<const char*>(source_name.data()), source_name.size() });
            if (!manifest)
            {
                throw avis::runtime_error{ std::move(manifest).error() };
            }
            return std::move(*manifest);
        }
    } // namespace detail
} // namespace avis::visualizer::rendering
