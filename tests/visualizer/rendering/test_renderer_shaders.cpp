// The renderer's shaders (docs/tasks/11-renderer.md §11.1): the shader directory, the startup load of every id in
// renderer_shaders.h, the binding probe against graphics::binding_layout (11 §13), and the shared C++/HLSL types

#include "helpers/rhi_fixture.h"
#include "helpers/runtime_fixture.h"
#include "helpers/temporary_directory.h"
#include "rendering/render_settings.h"
#include "rendering/renderer_shaders.h"
#include "rendering/shader_manifest_parser.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_bindings.h"
#include "avis/graphics/shader_library.h"
#include "avis/io/error.h"
#include "avis/threading/parallel_helpers.h"

#include <Eigen/Core>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <shared_types.hlsli>
#include <string>

#ifndef AVIS_TEST_RENDERER_SHADER_DIRECTORY
#    error "tests/CMakeLists.txt defines AVIS_TEST_RENDERER_SHADER_DIRECTORY, where the renderer's shaders are built"
#endif

using namespace avis;
using namespace avis::visualizer::rendering;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace
{
    const std::filesystem::path built_directory{ AVIS_TEST_RENDERER_SHADER_DIRECTORY };

    graphics::shader_manifest built_manifest()
    {
        const std::filesystem::path path = built_directory / shader_manifest_file_name;
        std::ifstream file{ path, std::ios::binary };
        REQUIRE(file.is_open());
        const std::string text{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
        auto manifest = parse_shader_manifest(text, path.generic_string());
        REQUIRE(manifest.has_value());
        return std::move(*manifest);
    }

    graphics::shader_bindings probe_bindings()
    {
        const graphics::shader_manifest manifest = built_manifest();
        const auto probe = std::ranges::find(
            manifest.entries, std::string{ shader_ids::binding_probe }, &graphics::shader_manifest_entry::id);
        REQUIRE(probe != manifest.entries.end());
        REQUIRE(probe->bindings.has_value());
        return *probe->bindings;
    }

    // HLSL pads a constant buffer's reflected size to whole 16-byte registers (18 §6)
    constexpr std::uint32_t padded(const std::size_t size)
    {
        return static_cast<std::uint32_t>((size + 15) / 16 * 16);
    }
} // namespace

TEST_CASE("the shader directory resolves next to the executable or from the settings", "[renderer][unit]")
{
    const std::filesystem::path executable_directory = std::filesystem::path{ "C:/avis/bin" };
    render_settings settings;
    CHECK(
        resolve_shader_directory(settings, executable_directory) ==
        std::filesystem::path{ "C:/avis/bin/data/shaders/visualizer" }.lexically_normal());

    settings.shader_directory = "D:/shaders/visualizer";
    CHECK(
        resolve_shader_directory(settings, executable_directory) ==
        std::filesystem::path{ "D:/shaders/visualizer" }.lexically_normal());

    settings.shader_directory = "../shaders";
    CHECK(
        resolve_shader_directory(settings, executable_directory) ==
        std::filesystem::path{ "C:/avis/shaders" }.lexically_normal());
}

TEST_CASE_METHOD(avis::tests::runtime_fixture, "load_renderer_shaders loads every renderer shader", "[renderer]")
{
    auto library = threading::sync_wait_result(load_renderer_shaders(built_directory));
    REQUIRE(library.has_value());
    CHECK(library->size() == built_manifest().entries.size());
    for (const std::string_view id : shader_ids::all)
    {
        CAPTURE(id);
        const auto entry = library->find(graphics::make_shader_id(id));
        REQUIRE(entry.has_value());
        CHECK(entry->bindings != nullptr);
    }
}

TEST_CASE_METHOD(avis::tests::runtime_fixture, "load_renderer_shaders reports a missing or bad manifest", "[renderer]")
{
    const avis::tests::temporary_directory directory;

    SECTION("a missing manifest is an IO error naming the directory")
    {
        auto library = threading::sync_wait_result(load_renderer_shaders(directory.path()));
        REQUIRE_FALSE(library.has_value());
        CHECK(io::is_error(library.error(), io::error_kind::not_found));
        CHECK_THAT(library.error().message, StartsWith("manifest.yaml not found in "));
        CHECK_THAT(library.error().message, ContainsSubstring(directory.path().generic_string()));
    }

    SECTION("a malformed manifest is the parse error with the manifest's path")
    {
        std::ofstream{ directory.file(shader_manifest_file_name) } << "avis_shader_manifest: 2\nshaders: {}\n";
        auto library = threading::sync_wait_result(load_renderer_shaders(directory.path()));
        REQUIRE_FALSE(library.has_value());
        CHECK(library.error().domain == error_domain::parse);
        CHECK_THAT(
            library.error().message,
            StartsWith((directory.path() / shader_manifest_file_name).generic_string() + ":2:10: shaders: "));
    }

    SECTION("a missing blob is load_shader_library's IO error")
    {
        std::ofstream{ directory.file(shader_manifest_file_name) } << R"(avis_shader_manifest: 2
shaders:
- {id: a.main.cs, file: a.main.cs.cso, stage: cs, xxh3: 0123456789abcdef,
   bindings: {constant_buffers: [], buffers: [], samplers: []}}
)";
        auto library = threading::sync_wait_result(load_renderer_shaders(directory.path()));
        REQUIRE_FALSE(library.has_value());
        CHECK(io::is_error(library.error(), io::error_kind::not_found));
        CHECK_THAT(library.error().message, ContainsSubstring("a.main.cs.cso"));
    }
}

TEST_CASE("the binding probe's bindings match graphics::binding_layout", "[renderer][unit]")
{
    namespace layout = graphics::binding_layout;
    const graphics::shader_bindings bindings = probe_bindings();

    // b0, b1, b2 in space0: the draw constants use the whole root-constant budget, and the view constants are
    // shared_types.hlsli's view_constants
    REQUIRE(bindings.constant_buffers.size() == 3);
    for (const graphics::constant_buffer_binding& binding : bindings.constant_buffers)
    {
        CHECK(binding.register_space == 0);
    }
    CHECK(bindings.constant_buffers[0].shader_register == layout::draw_constants_register);
    CHECK(bindings.constant_buffers[0].size == layout::draw_constant_count * 4);
    CHECK(bindings.constant_buffers[1].shader_register == layout::pass_constants_register);
    CHECK(bindings.constant_buffers[2].shader_register == layout::view_constants_register);
    CHECK(bindings.constant_buffers[2].size == padded(sizeof(gpu::view_constants)));

    // Nothing by register except the constants and the static samplers
    CHECK(bindings.buffers.empty());
    REQUIRE(bindings.samplers.size() == layout::static_sampler_count);
    for (std::uint32_t index = 0; index < layout::static_sampler_count; ++index)
    {
        CAPTURE(index);
        CHECK(bindings.samplers[index].shader_register == index);
        CHECK(bindings.samplers[index].register_space == 0);
    }
    CHECK(
        bindings.samplers[static_cast<std::uint32_t>(layout::static_sampler::shadow_comparison)].shader_register == 4);
}

TEST_CASE("the binding probe matches the standard root signature", "[renderer]")
{
    const graphics::shader_bindings bindings = probe_bindings();

    tests::test_rhi hardware_interface;
    graphics::root_signature_registry registry{ rhi::pipelines{ *hardware_interface } };
    const auto standard = registry.add("standard", graphics::binding_layout::standard_description());
    REQUIRE(standard.has_value());

    const graphics::root_signature_registry::stage_bindings stage{ .stage = graphics::shader_stage::compute,
                                                                   .bindings = &bindings };
    const auto compatible = registry.find_compatible({ &stage, 1 });
    REQUIRE(compatible.has_value());
    CHECK(*compatible == *standard);
}

TEST_CASE("shared types copy Eigen's column-major matrices as they are", "[renderer][unit]")
{
    // The layouts are static_asserted in shared_types.hlsli; this checks the matrix convention it documents
    Eigen::Matrix4f matrix;
    matrix << 1, 2, 3, 4, //
        5, 6, 7, 8, //
        9, 10, 11, 12, //
        13, 14, 15, 16;
    gpu::float4x4 copied{};
    static_assert(sizeof(copied) == sizeof(float) * 16);
    std::memcpy(&copied, matrix.data(), sizeof(copied));

    // Column 3 holds the translation of an affine matrix: (4, 8, 12, 16)
    CHECK(copied.columns[3].x == 4.0f);
    CHECK(copied.columns[3].y == 8.0f);
    CHECK(copied.columns[3].z == 12.0f);
    CHECK(copied.columns[3].w == 16.0f);
    CHECK(copied.columns[0].y == 5.0f);

    CHECK(gpu::vertex_attribute_position == 1u);
    CHECK(gpu::alpha_mode_blend == 2u);
}
