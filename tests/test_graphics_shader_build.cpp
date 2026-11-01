// The shader build round trip (docs/tasks/08-graphics-shaders-and-pipelines.md §8.7): avis_add_shaders compiled
// tests/shaders/graphics/ into AVIS_TEST_BUILT_SHADER_DIRECTORY with a manifest.yaml; this reads the manifest, loads
// the library, and checks the bindings and pipelines against the hand-written expectations

#include "helpers/graphics_test_shaders.h"
#include "helpers/rhi_fixture.h"
#include "helpers/runtime_fixture.h"
#include "rendering/shader_manifest_parser.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/pipeline_cache.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_bindings.h"
#include "avis/graphics/shader_library.h"
#include "avis/graphics/shader_loading.h"
#include "avis/graphics/shader_manifest.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/pipeline_factory.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/pipeline.h"
#include "avis/threading/parallel_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#ifndef AVIS_TEST_BUILT_SHADER_DIRECTORY
#    error "tests/CMakeLists.txt defines AVIS_TEST_BUILT_SHADER_DIRECTORY, the OUTPUT_DIRECTORY of avis_add_shaders"
#endif

using namespace avis;
namespace test_shaders = avis::tests::graphics_test_shaders;

namespace
{
    const std::filesystem::path built_directory{ AVIS_TEST_BUILT_SHADER_DIRECTORY };

    // The build's manifest, through the Visualizer's parser (12a), which reads format 2 (docs/tasks/11-renderer.md
    // §11.1): the library has no parser for it (D-73)
    graphics::shader_manifest read_manifest(const std::filesystem::path& path)
    {
        std::ifstream file{ path, std::ios::binary };
        REQUIRE(file.is_open());
        const std::string text{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
        auto manifest = visualizer::rendering::parse_shader_manifest(text, path.generic_string());
        REQUIRE(manifest.has_value());
        for (const graphics::shader_manifest_entry& entry : manifest->entries)
        {
            REQUIRE(entry.bindings.has_value()); // format 2
        }
        return std::move(*manifest);
    }

    const graphics::shader_manifest_entry&
        entry_of(const graphics::shader_manifest& manifest, const std::string_view id)
    {
        const auto found = std::ranges::find(manifest.entries, id, &graphics::shader_manifest_entry::id);
        REQUIRE(found != manifest.entries.end());
        return *found;
    }
} // namespace

TEST_CASE("the shader build writes a manifest of every test shader", "[graphics][shader_build]")
{
    const graphics::shader_manifest manifest = read_manifest(built_directory / "manifest.yaml");

    std::set<std::string> ids;
    for (const graphics::shader_manifest_entry& entry : manifest.entries)
    {
        ids.insert(entry.id);
    }
    CHECK(ids == std::set<std::string>(test_shaders::all_ids.begin(), test_shaders::all_ids.end()));

    SECTION("entries, stages and permutation defines")
    {
        const auto& tinted = entry_of(manifest, test_shaders::draw_pixel_tinted);
        CHECK(tinted.stage == graphics::shader_stage::pixel);
        CHECK(tinted.entry == "pixel_main");
        CHECK(tinted.blob == "pipeline_test_draw.pixel_main.tinted.ps.cso");
        CHECK(tinted.defines == std::vector<std::string>{ "TINTED" });

        const auto& plain = entry_of(manifest, test_shaders::draw_pixel);
        CHECK(plain.defines.empty());
        CHECK(entry_of(manifest, test_shaders::draw_vertex_tinted).defines == std::vector<std::string>{ "TINTED" });
        CHECK(entry_of(manifest, test_shaders::buffers_compute).stage == graphics::shader_stage::compute);
    }

    SECTION("the reflected bindings equal the hand-written ones")
    {
        for (const std::string_view id : test_shaders::all_ids)
        {
            CAPTURE(id);
            CHECK(entry_of(manifest, id).bindings == test_shaders::expected_bindings(id));
        }
    }
}

TEST_CASE_METHOD(
    avis::tests::runtime_fixture,
    "the built shaders load and make pipelines through the cache",
    "[graphics][shader_build]")
{
    const graphics::shader_manifest manifest = read_manifest(built_directory / "manifest.yaml");
    REQUIRE(std::ranges::all_of(manifest.entries, [](const auto& entry) { return entry.content_hash.has_value(); }));

    // Every hash matches, or the load fails with shader_hash_mismatch
    auto library = threading::sync_wait_result(graphics::load_shader_library(manifest, built_directory));
    REQUIRE(library.has_value());
    CHECK(library->size() == test_shaders::all_ids.size());

    tests::test_rhi hardware_interface;
    graphics::root_signature_registry registry{ rhi::pipelines{ *hardware_interface } };
    const auto standard = registry.add("standard", graphics::binding_layout::standard_description());
    REQUIRE(standard.has_value());
    const auto buffers = registry.add("buffers", test_shaders::buffers_description());
    REQUIRE(buffers.has_value());
    graphics::pipeline_cache cache{ rhi::pipelines{ *hardware_interface }, registry };

    rhi::d3d12::pipeline_factory& factory = hardware_interface->state().pipelines();
    const auto signature_of = [&](const auto pipeline) { return factory.find(pipeline)->root_signature; };

    rhi::graphics_pipeline_state state;
    state.depth.test = false;
    state.depth.write = false;
    state.render_target_count = 1;
    state.render_target_formats[0] = rhi::format::rgba8_unorm;

    for (const auto& [vertex, pixel] :
         { std::pair{ test_shaders::draw_vertex, test_shaders::draw_pixel },
           std::pair{ test_shaders::draw_vertex_tinted, test_shaders::draw_pixel_tinted } })
    {
        CAPTURE(pixel);
        const auto pipeline = cache.get_or_create(
            *library,
            graphics::graphics_pipeline_description{ .vertex_shader = graphics::make_shader_id(vertex),
                                                     .pixel_shader = graphics::make_shader_id(pixel),
                                                     .state = state,
                                                     .debug_name = pixel });
        REQUIRE(pipeline.has_value());
        CHECK(signature_of(*pipeline) == factory.find(*standard));
    }

    const auto standard_compute = cache.get_or_create(
        *library,
        graphics::compute_pipeline_description{ .compute_shader =
                                                    graphics::make_shader_id(test_shaders::binding_layout_compute) });
    REQUIRE(standard_compute.has_value());
    CHECK(signature_of(*standard_compute) == factory.find(*standard));

    // The shader that binds buffers by register gets the second signature
    const auto buffers_compute = cache.get_or_create(
        *library,
        graphics::compute_pipeline_description{ .compute_shader =
                                                    graphics::make_shader_id(test_shaders::buffers_compute) });
    REQUIRE(buffers_compute.has_value());
    CHECK(signature_of(*buffers_compute) == factory.find(*buffers));
}
