#ifndef AVIS_VISUALIZER_RENDERING_RENDER_SETTINGS_H
#define AVIS_VISUALIZER_RENDERING_RENDER_SETTINGS_H

// The renderer's settings: the `render` section of visualizer.yaml (docs/design/11-renderer.md §14). Reading the file
// itself is the Visualizer's job (12); this parses the section's node.

#include "avis/error.h"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace avis::visualizer::rendering
{
    enum class exposure_mode : std::uint8_t
    {
        automatic, // "auto": histogram and adaptation (11 §10.3)
        manual // fixed EV100
    };

    enum class tonemap_operator : std::uint8_t
    {
        aces,
        agx
    };

    struct shadow_settings
    {
        std::uint32_t cascades = 4; // 1-4: they use viewport slots 1-4 of the sort key (11 §4.2)
        std::uint32_t resolution = 2048; // texels per cascade side
        float distance = 200.0f; // metres

        friend bool operator==(const shadow_settings&, const shadow_settings&) = default;
    };

    struct ambient_occlusion_settings
    {
        bool half_resolution = true;
        float radius = 1.5f; // metres
        std::uint32_t slices = 2;
        std::uint32_t steps = 4; // per side
        float intensity = 1.0f;

        friend bool operator==(const ambient_occlusion_settings&, const ambient_occlusion_settings&) = default;
    };

    struct taa_settings
    {
        bool enabled = true;

        friend bool operator==(const taa_settings&, const taa_settings&) = default;
    };

    struct exposure_settings
    {
        exposure_mode mode = exposure_mode::automatic;
        float ev100 = 12.0f; // used when mode is manual

        friend bool operator==(const exposure_settings&, const exposure_settings&) = default;
    };

    struct progressive_settings
    {
        std::uint64_t vertex_budget = 20'000'000;
        float shadow_lod_scale = 0.25f; // in (0, 1]

        friend bool operator==(const progressive_settings&, const progressive_settings&) = default;
    };

    struct geometry_settings
    {
        std::uint32_t arena_mib = 768; // the renderer-owned geometry arena (11 §3.6)

        friend bool operator==(const geometry_settings&, const geometry_settings&) = default;
    };

    struct draw_sorting_settings
    {
        std::uint32_t opaque_depth_bands = 16; // a power of two, at most 2^24; 1 = no banding (11 §4.3)

        friend bool operator==(const draw_sorting_settings&, const draw_sorting_settings&) = default;
    };

    struct render_settings
    {
        std::uint32_t frames_in_flight = 2;
        bool async_compute = true;
        bool graph_aliasing = true;
        bool debug_layer = false;
        bool gpu_validation = false;
        float maximum_fps = 0.0f; // 0 = match the display refresh reported by Qt
        std::filesystem::path shader_directory; // empty = <executable directory>/data/shaders/visualizer
        shadow_settings shadows;
        ambient_occlusion_settings ao;
        taa_settings taa;
        exposure_settings exposure;
        tonemap_operator tonemapper = tonemap_operator::aces;
        progressive_settings progressive;
        geometry_settings geometry;
        draw_sorting_settings draw_sorting;

        friend bool operator==(const render_settings&, const render_settings&) = default;
    };

    // Parses the `render` section. A missing or null section, and every missing key, keeps the default of 11 §14;
    // unknown keys are ignored, as runtime_configuration::parse does. Errors are error_domain::parse and name the
    // position and the key path ("12:16: render.shadows.cascades: expected 1 to 4, got 6"). Pure: safe on any thread.
    result<render_settings> parse_render_settings(const YAML::Node& section);

    std::string_view to_string(exposure_mode mode) noexcept;
    std::string_view to_string(tonemap_operator value) noexcept;
} // namespace avis::visualizer::rendering

#endif
