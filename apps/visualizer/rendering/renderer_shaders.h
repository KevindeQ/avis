#ifndef AVIS_VISUALIZER_RENDERING_RENDERER_SHADERS_H
#define AVIS_VISUALIZER_RENDERING_RENDERER_SHADERS_H

// The scene renderer's shaders (docs/design/11-renderer.md §13, 12a §5): their ids, where they're deployed, and the
// startup load (read manifest.yaml, parse it on a parallel worker, load the library).

#include "rendering/render_settings.h"

#include "avis/error.h"
#include "avis/graphics/shader_library.h"
#include "avis/graphics/shader_loading.h"
#include "avis/graphics/shader_manifest.h"
#include "avis/io/file_operations.h"
#include "avis/runtime.h"
#include "avis/sender_of.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace avis::visualizer::rendering
{
    // The ids of every renderer shader, as avis_add_shaders names them: <file>.<entry>[.<permutation>].<stage>. Each
    // pass family adds its own as it comes (docs/tasks/11-renderer.md); the startup test checks that they all load.
    // The scene passes' vertex and pixel shaders aren't listed: pipeline_variant_registry builds their ids from a
    // pipeline set's key, and pipeline_variant_shader_ids() lists them (11.7).
    namespace shader_ids
    {
        // Declares every binding of bindless.hlsli, for the binding_layout check (11 §13)
        inline constexpr std::string_view binding_probe = "binding_probe.main.cs";

        // One mip of a decoded texture's chain, a 2 × 2 box in linear space (11.4)
        inline constexpr std::string_view generate_mips = "generate_mips.main.cs";

        // linear_depth and mip 0 of depth_mips from the depth buffer, and one further mip of depth_mips (11.10)
        inline constexpr std::string_view linearize_depth = "linearize_depth.main.cs";
        inline constexpr std::string_view depth_mips = "depth_mips.main.cs";
        // The light clusters' bounds and their lights (11.11)
        inline constexpr std::string_view cluster_build = "clusters.build_main.cs";
        inline constexpr std::string_view light_cull = "clusters.cull_main.cs";
        // GTAO and its edge-aware denoise (11.13)
        inline constexpr std::string_view gtao = "gtao.main.cs";
        inline constexpr std::string_view gtao_denoise = "gtao.denoise_main.cs";
        // hdr_color from the G-buffer, the lights and the baked environment (11.11)
        inline constexpr std::string_view deferred_lighting = "deferred_lighting.main.cs";
        // The sky where nothing was drawn (11.11)
        inline constexpr std::string_view sky_vertex = "sky.vertex_main.vs";
        inline constexpr std::string_view sky_pixel = "sky.pixel_main.ps";
        // The environment bake: the sky cubemap, the prefiltered cubemap, the SH9 irradiance, the BRDF LUT (11.11)
        inline constexpr std::string_view environment_sky = "environment_bake.sky_main.cs";
        inline constexpr std::string_view environment_prefilter = "environment_bake.prefilter_main.cs";
        inline constexpr std::string_view environment_irradiance = "environment_bake.irradiance_main.cs";
        inline constexpr std::string_view brdf_lut = "environment_bake.brdf_lut_main.cs";
        // The TAA resolve into the view's history (11.15)
        inline constexpr std::string_view taa = "taa.main.cs";
        // Automatic exposure: taa_output's histogram, then the adaptation (also the manual exposure and the
        // reset, 11.16)
        inline constexpr std::string_view exposure_histogram = "exposure.histogram_main.cs";
        inline constexpr std::string_view exposure_adapt = "exposure.adapt_main.cs";
        // ldr_color from taa_output: ACES or AgX, sRGB and the blue-noise dither (11.16)
        inline constexpr std::string_view tonemap = "tonemap.main.cs";
        // The selection outline into ldr_color, from selection_mask (11.17)
        inline constexpr std::string_view selection_outline = "selection_outline.main.cs";

        inline constexpr std::array all{ binding_probe,
                                         generate_mips,
                                         linearize_depth,
                                         depth_mips,
                                         cluster_build,
                                         light_cull,
                                         gtao,
                                         gtao_denoise,
                                         deferred_lighting,
                                         sky_vertex,
                                         sky_pixel,
                                         environment_sky,
                                         environment_prefilter,
                                         environment_irradiance,
                                         brdf_lut,
                                         taa,
                                         exposure_histogram,
                                         exposure_adapt,
                                         tonemap,
                                         selection_outline };
    } // namespace shader_ids

    // Where the build deploys the shaders, relative to the executable's directory
    inline constexpr std::string_view default_shader_subdirectory = "data/shaders/visualizer";

    // The manifest's file name inside the shader directory
    inline constexpr std::string_view shader_manifest_file_name = "manifest.yaml";

    // render.shader_directory when set (a relative one is relative to the executable's directory), otherwise
    // <executable directory>/data/shaders/visualizer (11 §14)
    std::filesystem::path
        resolve_shader_directory(const render_settings& settings, const std::filesystem::path& executable_directory);

    namespace detail
    {
        // Names the directory in a failed manifest read: a missing manifest suggests a missing deployment (12a §5).
        // Keeps the IO domain and code.
        error manifest_read_error(error failure, const std::filesystem::path& directory);

        // parse_shader_manifest over the file's bytes; throws avis::runtime_error with the parse error, which
        // into_internal_error turns back into it
        graphics::shader_manifest
            parse_manifest_or_throw(std::span<const std::byte> bytes, const std::filesystem::path& manifest_path);
    } // namespace detail

    // Loads the renderer's shader library from `directory` (12a §5): reads manifest.yaml through `context`, parses it
    // on a parallel worker, then graphics::load_shader_library reads every blob and checks its hash. Completes with
    // the library, or with the first error:
    // - the IO error of the manifest read, naming the directory ("manifest.yaml not found in <directory>; ...");
    // - the parse error, with the manifest's path, line, column and key path;
    // - load_shader_library's errors (a missing blob, a hash mismatch, a duplicate id).
    inline auto load_renderer_shaders(io::io_context& context, const std::filesystem::path& directory)
    {
        // Every capture copies: the operands of | aren't sequenced, so a lambda that moved from a variable could be
        // created before one that copies it
        const std::filesystem::path manifest_path = directory / shader_manifest_file_name;
        auto sender =
            io::read_file(context, manifest_path) |
            stdexec::let_error(
                [directory](error failure) noexcept
                { return stdexec::just_error(detail::manifest_read_error(std::move(failure), directory)); }) |
            stdexec::continues_on(stdexec::get_parallel_scheduler()) |
            stdexec::then([manifest_path](const std::vector<std::byte>& bytes)
                          { return detail::parse_manifest_or_throw(bytes, manifest_path); }) |
            stdexec::let_value([&context, directory](graphics::shader_manifest& manifest)
                               { return graphics::load_shader_library(context, std::move(manifest), directory); }) |
            threading::into_internal_error();
        static_assert(sender_of<decltype(sender), graphics::shader_library>);
        return sender;
    }

    // The same through the runtime's IO context
    inline auto load_renderer_shaders(const std::filesystem::path& directory)
    {
        return load_renderer_shaders(avis::runtime::io_context(), directory);
    }
} // namespace avis::visualizer::rendering

#endif
