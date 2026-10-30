// The shader build round trip (docs/tasks/08-graphics-shaders-and-pipelines.md §8.7): avis_add_shaders compiled
// tests/shaders/graphics/ into AVIS_TEST_BUILT_SHADER_DIRECTORY with a manifest.yaml; this reads the manifest, loads
// the library, and checks the bindings and pipelines against the hand-written expectations

#include "helpers/graphics_test_shaders.h"
#include "helpers/rhi_fixture.h"
#include "helpers/runtime_fixture.h"

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

#include <yaml-cpp/yaml.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
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

    graphics::shader_stage stage_of(const std::string& stage)
    {
        if (stage == "vs")
        {
            return graphics::shader_stage::vertex;
        }
        if (stage == "ps")
        {
            return graphics::shader_stage::pixel;
        }
        REQUIRE(stage == "cs");
        return graphics::shader_stage::compute;
    }

    // A minimal reader of the build's manifest (format 2), for this test only: the library has no parser for it
    // (D-73), the Visualizer has its own (12a)
    graphics::shader_manifest read_manifest(const std::filesystem::path& path)
    {
        const YAML::Node root = YAML::LoadFile(path.string());
        REQUIRE(root["avis_shader_manifest"].as<int>() == 2);

        graphics::shader_manifest manifest;
        for (const YAML::Node& shader : root["shaders"])
        {
            graphics::shader_manifest_entry entry{
                .id = shader["id"].as<std::string>(),
                .stage = stage_of(shader["stage"].as<std::string>()),
                .blob = shader["file"].as<std::string>(),
                .entry = shader["entry"].as<std::string>(),
                .content_hash = std::stoull(shader["xxh3"].as<std::string>(), nullptr, 16),
            };
            for (const YAML::Node& define : shader["defines"])
            {
                entry.defines.push_back(define.as<std::string>());
            }

            const YAML::Node bindings = shader["bindings"];
            REQUIRE(bindings.IsMap());
            graphics::shader_bindings read;
            for (const YAML::Node& binding : bindings["constant_buffers"])
            {
                read.constant_buffers.push_back(
                    { .shader_register = binding["register"].as<std::uint32_t>(),
                      .register_space = binding["space"].as<std::uint32_t>(),
                      .size = binding["size"].as<std::uint32_t>() });
            }
            for (const YAML::Node& binding : bindings["buffers"])
            {
                const std::string kind = binding["kind"].as<std::string>();
                REQUIRE((kind == "shader_resource" || kind == "unordered_access"));
                read.buffers.push_back(
                    { .kind = kind == "shader_resource" ? graphics::buffer_binding_kind::shader_resource :
                                                          graphics::buffer_binding_kind::unordered_access,
                      .shader_register = binding["register"].as<std::uint32_t>(),
                      .register_space = binding["space"].as<std::uint32_t>() });
            }
            for (const YAML::Node& binding : bindings["samplers"])
            {
                read.samplers.push_back(
                    { .shader_register = binding["register"].as<std::uint32_t>(),
                      .register_space = binding["space"].as<std::uint32_t>() });
            }
            entry.bindings = std::move(read);
            manifest.entries.push_back(std::move(entry));
        }
        return manifest;
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
