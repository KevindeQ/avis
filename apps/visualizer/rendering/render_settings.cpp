#include "rendering/render_settings.h"

#include "rendering/yaml_reading.h"

#include <array>
#include <bit>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        using yaml_reading::is_given;
        using yaml_reading::scalar;
        using yaml_reading::section;

        constexpr std::string_view section_path = "render";
        constexpr std::uint32_t maximum_cascades = 4;
        constexpr std::uint32_t maximum_depth_bands = 1u << 24;

        constexpr std::array exposure_modes{
            std::pair{ std::string_view{ "auto" }, exposure_mode::automatic },
            std::pair{ std::string_view{ "manual" }, exposure_mode::manual },
        };

        constexpr std::array tonemap_operators{
            std::pair{ std::string_view{ "aces" }, tonemap_operator::aces },
            std::pair{ std::string_view{ "agx" }, tonemap_operator::agx },
        };
    } // namespace

    result<render_settings> parse_render_settings(const YAML::Node& node)
    {
        render_settings settings;
        if (!is_given(node))
        {
            return settings;
        }

        const yaml_reading::reader reader;
        if (auto checked = reader.expect_map(node, section_path); !checked)
        {
            return std::unexpected{ std::move(checked).error() };
        }

        section render{ reader, node, std::string{ section_path } };
        render.read("frames_in_flight", settings.frames_in_flight);
        render.check("frames_in_flight", settings.frames_in_flight >= 1, "expected at least 1");
        render.read("async_compute", settings.async_compute);
        render.read("graph_aliasing", settings.graph_aliasing);
        render.read("debug_layer", settings.debug_layer);
        render.read("gpu_validation", settings.gpu_validation);
        render.read("maximum_fps", settings.maximum_fps);
        render.check("maximum_fps", settings.maximum_fps >= 0.0f, "expected 0 (display refresh) or more");
        render.read("shader_directory", settings.shader_directory);
        render.read_choice("tonemapper", settings.tonemapper, tonemap_operators);
        if (auto failure = render.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section shadows = render.child("shadows");
        shadows.read("cascades", settings.shadows.cascades);
        shadows.check(
            "cascades",
            settings.shadows.cascades >= 1 && settings.shadows.cascades <= maximum_cascades,
            std::format("expected 1 to {}, got {}", maximum_cascades, settings.shadows.cascades));
        shadows.read("resolution", settings.shadows.resolution);
        shadows.check("resolution", settings.shadows.resolution >= 1, "expected at least 1");
        shadows.read("distance", settings.shadows.distance);
        shadows.check("distance", settings.shadows.distance > 0.0f, "expected a positive distance");
        if (auto failure = shadows.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section ao = render.child("ao");
        ao.read("half_resolution", settings.ao.half_resolution);
        ao.read("radius", settings.ao.radius);
        ao.check("radius", settings.ao.radius > 0.0f, "expected a positive radius");
        ao.read("falloff", settings.ao.falloff);
        ao.check("falloff", settings.ao.falloff > 0.0f && settings.ao.falloff <= 1.0f, "expected a value in (0, 1]");
        ao.read("slices", settings.ao.slices);
        ao.check("slices", settings.ao.slices >= 1, "expected at least 1");
        ao.read("steps", settings.ao.steps);
        ao.check("steps", settings.ao.steps >= 1, "expected at least 1");
        ao.read("intensity", settings.ao.intensity);
        ao.check("intensity", settings.ao.intensity >= 0.0f, "expected 0 or more");
        if (auto failure = ao.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section taa = render.child("taa");
        taa.read("enabled", settings.taa.enabled);
        if (auto failure = taa.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section exposure = render.child("exposure");
        exposure.read_choice("mode", settings.exposure.mode, exposure_modes);
        exposure.read("ev100", settings.exposure.ev100);
        if (auto failure = exposure.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section progressive = render.child("progressive");
        progressive.read("vertex_budget", settings.progressive.vertex_budget);
        progressive.read("shadow_lod_scale", settings.progressive.shadow_lod_scale);
        progressive.check(
            "shadow_lod_scale",
            settings.progressive.shadow_lod_scale > 0.0f && settings.progressive.shadow_lod_scale <= 1.0f,
            std::format("expected a value in (0, 1], got {}", settings.progressive.shadow_lod_scale));
        if (auto failure = progressive.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section geometry = render.child("geometry");
        geometry.read("arena_mib", settings.geometry.arena_mib);
        geometry.check("arena_mib", settings.geometry.arena_mib >= 1, "expected at least 1");
        if (auto failure = geometry.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        section draw_sorting = render.child("draw_sorting");
        draw_sorting.read("opaque_depth_bands", settings.draw_sorting.opaque_depth_bands);
        const std::uint32_t bands = settings.draw_sorting.opaque_depth_bands;
        draw_sorting.check(
            "opaque_depth_bands",
            std::has_single_bit(bands) && bands <= maximum_depth_bands,
            std::format("expected a power of two from 1 to 2^24, got {}", bands));
        if (auto failure = draw_sorting.take_failure())
        {
            return std::unexpected{ std::move(*failure) };
        }

        return settings;
    }

    YAML::Node to_yaml(const render_settings& settings)
    {
        YAML::Node render{ YAML::NodeType::Map };
        render["frames_in_flight"] = scalar(settings.frames_in_flight);
        render["async_compute"] = scalar(settings.async_compute);
        render["graph_aliasing"] = scalar(settings.graph_aliasing);
        render["debug_layer"] = scalar(settings.debug_layer);
        render["gpu_validation"] = scalar(settings.gpu_validation);
        render["maximum_fps"] = scalar(settings.maximum_fps);
        render["shader_directory"] = yaml_reading::path_scalar(settings.shader_directory);

        YAML::Node shadows = yaml_reading::flow_map();
        shadows["cascades"] = scalar(settings.shadows.cascades);
        shadows["resolution"] = scalar(settings.shadows.resolution);
        shadows["distance"] = scalar(settings.shadows.distance);
        render["shadows"] = shadows;

        YAML::Node ao = yaml_reading::flow_map();
        ao["half_resolution"] = scalar(settings.ao.half_resolution);
        ao["radius"] = scalar(settings.ao.radius);
        ao["falloff"] = scalar(settings.ao.falloff);
        ao["slices"] = scalar(settings.ao.slices);
        ao["steps"] = scalar(settings.ao.steps);
        ao["intensity"] = scalar(settings.ao.intensity);
        render["ao"] = ao;

        YAML::Node taa = yaml_reading::flow_map();
        taa["enabled"] = scalar(settings.taa.enabled);
        render["taa"] = taa;

        YAML::Node exposure = yaml_reading::flow_map();
        exposure["mode"] = scalar(to_string(settings.exposure.mode));
        exposure["ev100"] = scalar(settings.exposure.ev100);
        render["exposure"] = exposure;

        render["tonemapper"] = scalar(to_string(settings.tonemapper));

        YAML::Node progressive = yaml_reading::flow_map();
        progressive["vertex_budget"] = scalar(settings.progressive.vertex_budget);
        progressive["shadow_lod_scale"] = scalar(settings.progressive.shadow_lod_scale);
        render["progressive"] = progressive;

        YAML::Node geometry = yaml_reading::flow_map();
        geometry["arena_mib"] = scalar(settings.geometry.arena_mib);
        render["geometry"] = geometry;

        YAML::Node draw_sorting = yaml_reading::flow_map();
        draw_sorting["opaque_depth_bands"] = scalar(settings.draw_sorting.opaque_depth_bands);
        render["draw_sorting"] = draw_sorting;
        return render;
    }

    std::string_view to_string(const exposure_mode mode) noexcept
    {
        return mode == exposure_mode::manual ? "manual" : "auto";
    }

    std::string_view to_string(const tonemap_operator value) noexcept
    {
        return value == tonemap_operator::agx ? "agx" : "aces";
    }
} // namespace avis::visualizer::rendering
