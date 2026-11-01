// render_settings (docs/tasks/11-renderer.md §11.1): the defaults of docs/design/11-renderer.md §14 and the validation
// rules, each failing with its key path

#include "rendering/render_settings.h"

#include <yaml-cpp/yaml.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <string>

using namespace avis::visualizer::rendering;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace
{
    avis::result<render_settings> parse(const std::string& yaml)
    {
        return parse_render_settings(YAML::Load(yaml)["render"]);
    }

    // The render section of 11 §14, as written there
    constexpr const char* design_section = R"(
render:
  frames_in_flight: 2
  async_compute: true
  graph_aliasing: true
  debug_layer: false
  gpu_validation: false
  maximum_fps: 0                 # 0 = match display refresh reported by Qt
  shader_directory: ""           # empty = <executable directory>/data/shaders/visualizer
  shadows: { cascades: 4, resolution: 2048, distance: 200.0 }
  ao: { half_resolution: true, radius: 1.5, slices: 2, steps: 4, intensity: 1.0 }
  taa: { enabled: true }
  exposure: { mode: auto, ev100: 12.0 }
  tonemapper: aces               # aces | agx
  progressive: { vertex_budget: 20000000, shadow_lod_scale: 0.25 }
  geometry: { arena_mib: 768 }
  draw_sorting: { opaque_depth_bands: 16 }
)";

    void check_rejected(const std::string& yaml, const std::string& key_path)
    {
        CAPTURE(yaml);
        const auto settings = parse(yaml);
        REQUIRE_FALSE(settings.has_value());
        CHECK(settings.error().domain == avis::error_domain::parse);
        CHECK_THAT(settings.error().message, ContainsSubstring(key_path + ": "));
    }
} // namespace

TEST_CASE("render settings default to 11 §14", "[renderer][unit]")
{
    const render_settings defaults;
    CHECK(defaults.frames_in_flight == 2);
    CHECK(defaults.async_compute);
    CHECK(defaults.graph_aliasing);
    CHECK_FALSE(defaults.debug_layer);
    CHECK_FALSE(defaults.gpu_validation);
    CHECK(defaults.maximum_fps == 0.0f);
    CHECK(defaults.shader_directory.empty());
    CHECK(defaults.shadows.cascades == 4);
    CHECK(defaults.shadows.resolution == 2048);
    CHECK(defaults.shadows.distance == 200.0f);
    CHECK(defaults.ao.half_resolution);
    CHECK(defaults.ao.radius == 1.5f);
    CHECK(defaults.ao.slices == 2);
    CHECK(defaults.ao.steps == 4);
    CHECK(defaults.ao.intensity == 1.0f);
    CHECK(defaults.taa.enabled);
    CHECK(defaults.exposure.mode == exposure_mode::automatic);
    CHECK(defaults.exposure.ev100 == 12.0f);
    CHECK(defaults.tonemapper == tonemap_operator::aces);
    CHECK(defaults.progressive.vertex_budget == 20'000'000);
    CHECK(defaults.progressive.shadow_lod_scale == 0.25f);
    CHECK(defaults.geometry.arena_mib == 768);
    CHECK(defaults.draw_sorting.opaque_depth_bands == 16);

    SECTION("the section as 11 §14 writes it parses to the defaults")
    {
        const auto settings = parse(design_section);
        REQUIRE(settings.has_value());
        CHECK(*settings == defaults);
    }

    SECTION("a missing, null or empty section keeps the defaults")
    {
        for (const char* yaml : { "other: 1", "render:", "render: ~", "render: {}" })
        {
            CAPTURE(yaml);
            const auto settings = parse(yaml);
            REQUIRE(settings.has_value());
            CHECK(*settings == defaults);
        }
    }

    SECTION("missing keys keep their defaults, unknown keys are ignored")
    {
        const auto settings = parse("render: { shadows: { cascades: 2 }, future_key: 7, ao: { future: x } }");
        REQUIRE(settings.has_value());
        render_settings expected;
        expected.shadows.cascades = 2;
        CHECK(*settings == expected);
    }
}

TEST_CASE("render settings read every key", "[renderer][unit]")
{
    const auto settings = parse(R"(
render:
  frames_in_flight: 3
  async_compute: false
  graph_aliasing: false
  debug_layer: true
  gpu_validation: true
  maximum_fps: 144.5
  shader_directory: "shaders/custom"
  shadows: { cascades: 1, resolution: 1024, distance: 50 }
  ao: { half_resolution: false, radius: 0.5, slices: 3, steps: 6, intensity: 2.0 }
  taa: { enabled: false }
  exposure: { mode: manual, ev100: 9.5 }
  tonemapper: agx
  progressive: { vertex_budget: 5000000000, shadow_lod_scale: 1 }
  geometry: { arena_mib: 1024 }
  draw_sorting: { opaque_depth_bands: 1 }
)");
    REQUIRE(settings.has_value());
    CHECK(settings->frames_in_flight == 3);
    CHECK_FALSE(settings->async_compute);
    CHECK_FALSE(settings->graph_aliasing);
    CHECK(settings->debug_layer);
    CHECK(settings->gpu_validation);
    CHECK(settings->maximum_fps == 144.5f);
    CHECK(settings->shader_directory == std::filesystem::path{ "shaders/custom" });
    CHECK(settings->shadows.cascades == 1);
    CHECK(settings->shadows.resolution == 1024);
    CHECK(settings->shadows.distance == 50.0f);
    CHECK_FALSE(settings->ao.half_resolution);
    CHECK(settings->ao.radius == 0.5f);
    CHECK(settings->ao.slices == 3);
    CHECK(settings->ao.steps == 6);
    CHECK(settings->ao.intensity == 2.0f);
    CHECK_FALSE(settings->taa.enabled);
    CHECK(settings->exposure.mode == exposure_mode::manual);
    CHECK(settings->exposure.ev100 == 9.5f);
    CHECK(settings->tonemapper == tonemap_operator::agx);
    CHECK(settings->progressive.vertex_budget == 5'000'000'000);
    CHECK(settings->progressive.shadow_lod_scale == 1.0f);
    CHECK(settings->geometry.arena_mib == 1024);
    CHECK(settings->draw_sorting.opaque_depth_bands == 1);
}

TEST_CASE("render settings validation rejects bad values with the key path", "[renderer][unit]")
{
    SECTION("shadows.cascades is between 1 and 4")
    {
        check_rejected("render: { shadows: { cascades: 0 } }", "render.shadows.cascades");
        check_rejected("render: { shadows: { cascades: 5 } }", "render.shadows.cascades");
        for (const char* yaml : { "render: { shadows: { cascades: 1 } }", "render: { shadows: { cascades: 4 } }" })
        {
            CHECK(parse(yaml).has_value());
        }
    }

    SECTION("draw_sorting.opaque_depth_bands is a power of two, at most 2^24")
    {
        check_rejected("render: { draw_sorting: { opaque_depth_bands: 0 } }", "render.draw_sorting.opaque_depth_bands");
        check_rejected(
            "render: { draw_sorting: { opaque_depth_bands: 12 } }", "render.draw_sorting.opaque_depth_bands");
        check_rejected(
            "render: { draw_sorting: { opaque_depth_bands: 33554432 } }", "render.draw_sorting.opaque_depth_bands");
        CHECK(parse("render: { draw_sorting: { opaque_depth_bands: 16777216 } }").has_value());
        CHECK(parse("render: { draw_sorting: { opaque_depth_bands: 1 } }").has_value());
    }

    SECTION("progressive.shadow_lod_scale is in (0, 1]")
    {
        check_rejected("render: { progressive: { shadow_lod_scale: 0 } }", "render.progressive.shadow_lod_scale");
        check_rejected("render: { progressive: { shadow_lod_scale: -0.5 } }", "render.progressive.shadow_lod_scale");
        check_rejected("render: { progressive: { shadow_lod_scale: 1.01 } }", "render.progressive.shadow_lod_scale");
        CHECK(parse("render: { progressive: { shadow_lod_scale: 1.0 } }").has_value());
        CHECK(parse("render: { progressive: { shadow_lod_scale: 0.001 } }").has_value());
    }

    SECTION("tonemapper is aces or agx, exposure.mode is auto or manual")
    {
        check_rejected("render: { tonemapper: reinhard }", "render.tonemapper");
        check_rejected("render: { tonemapper: [aces] }", "render.tonemapper");
        check_rejected("render: { exposure: { mode: automatic } }", "render.exposure.mode");
        check_rejected("render: { exposure: { mode: 1 } }", "render.exposure.mode");
    }

    SECTION("types")
    {
        check_rejected("render: { frames_in_flight: two }", "render.frames_in_flight");
        check_rejected("render: { frames_in_flight: -1 }", "render.frames_in_flight");
        check_rejected("render: { async_compute: maybe }", "render.async_compute");
        check_rejected("render: { maximum_fps: fast }", "render.maximum_fps");
        check_rejected("render: { shadows: 3 }", "render.shadows");
        check_rejected("render: { shadows: { distance: [1] } }", "render.shadows.distance");
        check_rejected("render: { shader_directory: { a: 1 } }", "render.shader_directory");
    }

    SECTION("the message starts with the 1-based position of the value")
    {
        const auto settings = parse("render:\n  shadows:\n    cascades: 9\n");
        REQUIRE_FALSE(settings.has_value());
        CHECK_THAT(settings.error().message, StartsWith("3:15: render.shadows.cascades: expected 1 to 4, got 9"));
    }

    SECTION("a section that isn't a mapping")
    {
        const auto settings = parse_render_settings(YAML::Load("[1, 2]"));
        REQUIRE_FALSE(settings.has_value());
        CHECK_THAT(settings.error().message, ContainsSubstring("render: expected a mapping"));
    }
}
