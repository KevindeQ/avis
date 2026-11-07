// visualizer_configuration (docs/tasks/12-visualizer.md §12.1): visualizer.yaml's defaults, the validation of each
// section with its key path, the round trip through to_yaml_text, the missing-file case and the command line

#include "helpers/temporary_directory.h"
#include "visualizer_configuration.h"

#include "avis/io/error.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace avis::visualizer;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace
{
    using avis::scene::entity_category;

    constexpr std::size_t index_of(const entity_category category)
    {
        return static_cast<std::size_t>(category);
    }

    // The YAML blocks of the design, as written there: 11 §14 (render), 12 §6.3 (screenshots) and 12 §9
    // (representation)
    constexpr const char* design_blocks = R"(
render:
  frames_in_flight: 2
  async_compute: true
  graph_aliasing: true
  debug_layer: false
  gpu_validation: false
  maximum_fps: 0                 # 0 = match display refresh reported by Qt
  shader_directory: ""           # empty = <executable directory>/data/shaders/visualizer
  shadows: { cascades: 4, resolution: 2048, distance: 200.0 }   # cascades <= 4
  ao: { half_resolution: true, radius: 1.5, falloff: 0.4, slices: 2, steps: 4, intensity: 1.0 }   # falloff: fraction of the radius
  taa: { enabled: true }
  exposure: { mode: auto, ev100: 12.0 }
  tonemapper: aces               # aces | agx
  progressive: { vertex_budget: 20000000, shadow_lod_scale: 0.25 }
  geometry: { arena_mib: 768 }   # renderer-owned geometry arena
  draw_sorting: { opaque_depth_bands: 16 }   # power of two; 1 = no depth banding for opaque groups
screenshots:
  folder: ""                       # empty = %USERPROFILE%/Pictures/Avis
  custom: { width: 3840, height: 2160 }   # last used custom resolution
  include_debug_view: false
representation:
  boxes: { edge_width: 2.0 }
  lidar: { maximum_points_per_sweep: 200000 }
  geometry: { budget_mib: 512 }       # CPU-side budget for dynamic meshes (belief geometry, lidar sweeps, trails)
  labels:
    enabled: true
    maximum_count: 200                # nearest to the primary vehicle first
    font: data/assets/fonts/labels    # atlas and metrics, 1x and 2x
    style: { pixel_height: 14.0, maximum_view_distance: 80.0 }
    templates:                        # per entity category; "" = no label
      vehicle: "{name}"
      object: "{name} · {property:perception.speed:.1f}"
      sensor: ""
  trails:                             # per entity category; 0 = off
    vehicle: { duration_seconds: 30.0, minimum_spacing: 0.25 }
    object: { duration_seconds: 0.0 }
  stale: { enabled: false, after_seconds: 2.0, categories: [object] }
)";

    avis::result<visualizer_configuration> parse(const std::string& yaml)
    {
        return parse_visualizer_configuration(yaml, "visualizer.yaml");
    }

    void check_rejected(const std::string& yaml, const std::string& key_path)
    {
        CAPTURE(yaml);
        const auto configuration = parse(yaml);
        REQUIRE_FALSE(configuration.has_value());
        CHECK(configuration.error().domain == avis::error_domain::parse);
        CHECK_THAT(configuration.error().message, StartsWith("visualizer.yaml:"));
        CHECK_THAT(configuration.error().message, ContainsSubstring(key_path + ": "));
    }

    std::string read_text(const std::filesystem::path& path)
    {
        std::ifstream file{ path, std::ios::binary };
        return std::string{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
    }

    void write_text(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream file{ path, std::ios::binary | std::ios::trunc };
        file << text;
    }

    // A configuration with a non-default value in every section
    visualizer_configuration changed_configuration()
    {
        visualizer_configuration configuration;
        configuration.runtime.io.threads = 3;
        configuration.runtime.parallel.workers = 5;
        configuration.runtime.logging.default_level = avis::log_level::debug;
        configuration.runtime.logging.levels.emplace("rhi", avis::log_level::warn);
        configuration.runtime.logging.file = std::filesystem::path{ "logs/visualizer.log" };
        configuration.render.frames_in_flight = 3;
        configuration.render.ao.falloff = 0.3f;
        configuration.render.tonemapper = avis::visualizer::rendering::tonemap_operator::agx;
        configuration.render.shader_directory = std::filesystem::path{ u8"shaders/\u00e9t\u00e9" };
        configuration.representation.boxes.edge_width = 1.5f;
        configuration.representation.labels.templates[index_of(entity_category::sensor)] = "{name}";
        configuration.representation.trails[index_of(entity_category::object)].duration_seconds = 5.0f;
        configuration.representation.stale.categories = { entity_category::object, entity_category::vehicle };
        configuration.screenshots.custom = { .width = 1920, .height = 1080 };
        configuration.screenshots.folder = std::filesystem::path{ "D:/Pictures/Avis" };
        configuration.lockstep.node = YAML::Load("{ communication_timeout: 5s, long_step_notice: 2s }");
        configuration.network.node = YAML::Load("scouting: { timeout: 3s }");
        configuration.ui.last_driver = "driver-0";
        configuration.ui.recent_files = { std::filesystem::path{ "E:/scenarios/a.yaml" },
                                          std::filesystem::path{ "E:/scenarios/b.yaml" } };
        return configuration;
    }
} // namespace

TEST_CASE("the configuration's defaults are the design's YAML blocks", "[visualizer][unit]")
{
    const visualizer_configuration defaults;

    SECTION("an empty file gives the defaults")
    {
        const auto empty = parse("");
        REQUIRE(empty.has_value());
        CHECK(*empty == defaults);
    }

    SECTION("the blocks of 11 §14, 12 §6.3 and 12 §9 parse to the defaults")
    {
        const auto design = parse(design_blocks);
        REQUIRE(design.has_value());
        CHECK(design->render == defaults.render);
        CHECK(design->screenshots == defaults.screenshots);
        CHECK(design->representation == defaults.representation);
        CHECK(*design == defaults);
    }

    SECTION("the values the design names")
    {
        CHECK(defaults.screenshots.folder.empty());
        CHECK(defaults.screenshots.custom == screenshot_settings::size{ .width = 3840, .height = 2160 });
        CHECK_FALSE(defaults.screenshots.include_debug_view);

        const auto& representation = defaults.representation;
        CHECK(representation.boxes.edge_width == 2.0f);
        CHECK(representation.lidar.maximum_points_per_sweep == 200'000);
        CHECK(representation.geometry.budget_mib == 512);
        CHECK(representation.labels.enabled);
        CHECK(representation.labels.maximum_count == 200);
        CHECK(representation.labels.font == std::filesystem::path{ "data/assets/fonts/labels" });
        CHECK(representation.labels.style.pixel_height == 14.0f);
        CHECK(representation.labels.style.maximum_view_distance == 80.0f);
        CHECK(representation.labels.templates[index_of(entity_category::vehicle)] == "{name}");
        CHECK(representation.labels.templates[index_of(entity_category::sensor)].empty());
        CHECK(representation.trails[index_of(entity_category::vehicle)].duration_seconds == 30.0f);
        CHECK(representation.trails[index_of(entity_category::vehicle)].minimum_spacing == 0.25f);
        CHECK(representation.trails[index_of(entity_category::object)].duration_seconds == 0.0f);
        CHECK_FALSE(representation.stale.enabled);
        CHECK(representation.stale.after_seconds == 2.0f);
        CHECK(representation.stale.categories == std::vector{ entity_category::object });

        CHECK(defaults.lockstep.node.IsNull());
        CHECK(defaults.network.node.IsNull());
        CHECK(defaults.ui.last_driver.empty());
        CHECK(defaults.ui.recent_files.empty());
    }
}

TEST_CASE("every configuration section rejects a bad value with its key path", "[visualizer][unit]")
{
    SECTION("runtime and logging")
    {
        check_rejected("runtime: { io_threads: -1 }", "runtime.io_threads");
        check_rejected("logging: { level: loud }", "logging.level");
    }

    SECTION("render")
    {
        check_rejected("render: { shadows: { cascades: 6 } }", "render.shadows.cascades");
        check_rejected("render: { tonemapper: reinhard }", "render.tonemapper");
    }

    SECTION("representation")
    {
        check_rejected("representation: [1, 2]", "representation");
        check_rejected("representation: { boxes: { edge_width: 0 } }", "representation.boxes.edge_width");
        check_rejected(
            "representation: { lidar: { maximum_points_per_sweep: lots } }",
            "representation.lidar.maximum_points_per_sweep");
        check_rejected("representation: { geometry: { budget_mib: 0 } }", "representation.geometry.budget_mib");
        check_rejected("representation: { labels: { maximum_count: -1 } }", "representation.labels.maximum_count");
        check_rejected(
            "representation: { labels: { style: { pixel_height: -2 } } }", "representation.labels.style.pixel_height");
        check_rejected("representation: { labels: { templates: { truck: x } } }", "representation.labels.templates");
        check_rejected("representation: { labels: { templates: { helper: x } } }", "representation.labels.templates");
        check_rejected(
            "representation: { labels: { templates: { vehicle: [x] } } }", "representation.labels.templates.vehicle");
        check_rejected(
            "representation: { trails: { vehicle: { duration_seconds: -1 } } }",
            "representation.trails.vehicle.duration_seconds");
        check_rejected(
            "representation: { trails: { object: { minimum_spacing: 0 } } }",
            "representation.trails.object.minimum_spacing");
        check_rejected("representation: { stale: { after_seconds: 0 } }", "representation.stale.after_seconds");
        check_rejected("representation: { stale: { categories: object } }", "representation.stale.categories");
        check_rejected(
            "representation: { stale: { categories: [object, object] } }", "representation.stale.categories[1]");
    }

    SECTION("screenshots")
    {
        check_rejected("screenshots: { custom: { width: 0 } }", "screenshots.custom.width");
        check_rejected("screenshots: { custom: { height: 10000 } }", "screenshots.custom.height");
        check_rejected("screenshots: { include_debug_view: maybe }", "screenshots.include_debug_view");
        check_rejected("screenshots: { folder: [a] }", "screenshots.folder");
    }

    SECTION("lockstep and network, kept as written, must be mappings")
    {
        check_rejected("lockstep: 5s", "lockstep");
        check_rejected("network: [peer]", "network");
    }

    SECTION("ui")
    {
        check_rejected("ui: { last_driver: [a] }", "ui.last_driver");
        check_rejected("ui: { recent_files: a.yaml }", "ui.recent_files");
        check_rejected("ui: { recent_files: [a.yaml, [b]] }", "ui.recent_files[1]");
    }

    SECTION("malformed YAML reports its position")
    {
        const auto configuration = parse("render:\n  shadows: { cascades: 4\n");
        REQUIRE_FALSE(configuration.has_value());
        CHECK(configuration.error().domain == avis::error_domain::parse);
        CHECK_THAT(configuration.error().message, StartsWith("visualizer.yaml:3:"));
    }
}

TEST_CASE("per-category representation settings merge with the defaults", "[visualizer][unit]")
{
    const auto configuration = parse(R"(
representation:
  labels:
    templates: { sensor: "{name}", vehicle: }
  trails:
    object: { duration_seconds: 5.0 }
  stale: { categories: [vehicle, geometry] }
)");
    REQUIRE(configuration.has_value());
    const auto& representation = configuration->representation;
    const visualizer_configuration defaults;

    // A template given replaces that category's, an empty value removes it, and the others keep their defaults
    CHECK(representation.labels.templates[index_of(entity_category::sensor)] == "{name}");
    CHECK(representation.labels.templates[index_of(entity_category::vehicle)].empty());
    CHECK(
        representation.labels.templates[index_of(entity_category::object)] ==
        defaults.representation.labels.templates[index_of(entity_category::object)]);

    // A trail's missing keys keep their defaults
    CHECK(representation.trails[index_of(entity_category::object)].duration_seconds == 5.0f);
    CHECK(representation.trails[index_of(entity_category::object)].minimum_spacing == 0.25f);
    CHECK(representation.trails[index_of(entity_category::vehicle)].duration_seconds == 30.0f);

    // The stale list replaces the default list
    CHECK(representation.stale.categories == std::vector{ entity_category::vehicle, entity_category::geometry });
}

TEST_CASE("the configuration round-trips through its YAML text", "[visualizer][unit]")
{
    SECTION("the defaults")
    {
        const visualizer_configuration defaults;
        const auto reread = parse(to_yaml_text(defaults));
        REQUIRE(reread.has_value());
        CHECK(*reread == defaults);
    }

    SECTION("a value changed in every section")
    {
        const visualizer_configuration configuration = changed_configuration();
        const std::string text = to_yaml_text(configuration);
        CAPTURE(text);
        const auto reread = parse(text);
        REQUIRE(reread.has_value());
        CHECK(reread->runtime.io.threads == 3);
        CHECK(reread->render == configuration.render);
        CHECK(reread->representation == configuration.representation);
        CHECK(reread->lockstep == configuration.lockstep);
        CHECK(reread->ui == configuration.ui);
        CHECK(*reread == configuration);
        CHECK_FALSE(*reread == visualizer_configuration{});
    }

    SECTION("unknown sections are kept, in their order, after the known ones")
    {
        const auto configuration = parse(R"(
experiments: { fast_path: true, names: [a, b] }
render: { frames_in_flight: 3 }
notes: "kept as written"
)");
        REQUIRE(configuration.has_value());
        CHECK(configuration->render.frames_in_flight == 3);
        REQUIRE(configuration->unknown_sections.node.IsMap());
        CHECK(configuration->unknown_sections.node.size() == 2);

        const std::string text = to_yaml_text(*configuration);
        CAPTURE(text);
        const auto reread = parse(text);
        REQUIRE(reread.has_value());
        CHECK(*reread == *configuration);
        CHECK(reread->unknown_sections.node["experiments"]["names"][1].as<std::string>() == "b");
        CHECK(reread->unknown_sections.node["notes"].as<std::string>() == "kept as written");
        CHECK(text.find("experiments") < text.find("notes"));
        CHECK(text.find("ui:") < text.find("experiments"));
    }
}

TEST_CASE("a missing configuration file is written with the defaults", "[visualizer][unit]")
{
    const avis::tests::temporary_directory directory;
    const std::filesystem::path path = directory.path() / "Avis" / "visualizer.yaml";

    const auto first = load_visualizer_configuration(path);
    REQUIRE(first.has_value());
    CHECK(first->created);
    CHECK_FALSE(first->save_failure.has_value());
    CHECK(first->configuration == visualizer_configuration{});
    REQUIRE(std::filesystem::is_regular_file(path));
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));

    // Loading the written file again gives the same configuration, and doesn't rewrite it
    const std::string written = read_text(path);
    const auto second = load_visualizer_configuration(path);
    REQUIRE(second.has_value());
    CHECK_FALSE(second->created);
    CHECK(second->configuration == first->configuration);
    CHECK(read_text(path) == written);

    SECTION("a saved configuration loads back")
    {
        const visualizer_configuration configuration = changed_configuration();
        REQUIRE(save_visualizer_configuration(configuration, path).has_value());
        const auto loaded = load_visualizer_configuration(path);
        REQUIRE(loaded.has_value());
        CHECK(loaded->configuration == configuration);
    }

    SECTION("a malformed file is a parse error that names the file")
    {
        write_text(path, "render: { frames_in_flight: 0 }\n");
        const auto loaded = load_visualizer_configuration(path);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().domain == avis::error_domain::parse);
        CHECK_THAT(loaded.error().message, ContainsSubstring("visualizer.yaml:1:"));
        CHECK_THAT(loaded.error().message, ContainsSubstring("render.frames_in_flight: "));
    }
}

TEST_CASE("a configuration that can't be written is still loaded", "[visualizer][unit]")
{
    const avis::tests::temporary_directory directory;

    // The configuration's directory is a file, so it can't be created
    const std::filesystem::path blocker = directory.path() / "Avis";
    write_text(blocker, "not a directory");
    const auto loaded = load_visualizer_configuration(blocker / "visualizer.yaml");
    REQUIRE(loaded.has_value());
    CHECK(loaded->created);
    REQUIRE(loaded->save_failure.has_value());
    CHECK(loaded->save_failure->domain == avis::error_domain::io);
    CHECK(loaded->configuration == visualizer_configuration{});
}

TEST_CASE("the log file comes from the configuration or sits next to the executable", "[visualizer][unit]")
{
    const std::filesystem::path executable_directory{ "C:/Avis/bin" };
    visualizer_configuration configuration;
    CHECK(resolve_log_file(configuration, executable_directory) == executable_directory / "visualizer.log");

    configuration.runtime.logging.file = std::filesystem::path{ "logs/visualizer.log" };
    CHECK(resolve_log_file(configuration, executable_directory) == executable_directory / "logs/visualizer.log");

    configuration.runtime.logging.file = std::filesystem::path{ "D:/logs/avis.log" };
    CHECK(resolve_log_file(configuration, executable_directory) == std::filesystem::path{ "D:/logs/avis.log" });
}

TEST_CASE("the command line names the configuration file", "[visualizer][unit]")
{
    const auto parse_arguments = [](const std::vector<std::wstring>& arguments)
    { return parse_command_line(arguments); };

    SECTION("no option keeps the default")
    {
        const auto options = parse_arguments({ L"-platform", L"windows", L"scenario.yaml" });
        REQUIRE(options.has_value());
        CHECK_FALSE(options->configuration.has_value());
    }

    SECTION("both spellings, the last one winning")
    {
        const auto separate = parse_arguments({ L"--configuration", L"D:/a b/visualizer.yaml" });
        REQUIRE(separate.has_value());
        CHECK(separate->configuration == std::filesystem::path{ L"D:/a b/visualizer.yaml" });

        const auto joined = parse_arguments({ L"--configuration=one.yaml", L"--configuration=\u00e9t\u00e9.yaml" });
        REQUIRE(joined.has_value());
        CHECK(joined->configuration == std::filesystem::path{ L"\u00e9t\u00e9.yaml" });
    }

    SECTION("a missing path is an error")
    {
        for (const std::vector<std::wstring>& arguments :
             { std::vector<std::wstring>{ L"--configuration" }, std::vector<std::wstring>{ L"--configuration=" } })
        {
            const auto options = parse_arguments(arguments);
            REQUIRE_FALSE(options.has_value());
            CHECK(options.error().code == static_cast<std::int32_t>(avis::generic_error_kind::invalid_configuration));
        }
    }

    SECTION("a longer option isn't taken for it")
    {
        const auto options = parse_arguments({ L"--configurations=x.yaml" });
        REQUIRE(options.has_value());
        CHECK_FALSE(options->configuration.has_value());
    }
}

TEST_CASE("the default configuration path is under APPDATA", "[visualizer][unit]")
{
    const std::filesystem::path path = default_configuration_path();
    CHECK(path.filename() == "visualizer.yaml");
    CHECK(path.parent_path().filename() == "Avis");
}
