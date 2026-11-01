// The shader manifest parser (docs/design/12a-visualizer-shader-manifest.md §6, docs/tasks/11-renderer.md §11.1):
// hand-written cases for formats 1 and 2, every rule with its position and key path, and the manifest the build wrote
// for the renderer's shaders

#include "rendering/renderer_shaders.h"
#include "rendering/shader_manifest_parser.h"

#include "avis/graphics/shader_manifest.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#ifndef AVIS_TEST_RENDERER_SHADER_DIRECTORY
#    error "tests/CMakeLists.txt defines AVIS_TEST_RENDERER_SHADER_DIRECTORY, where the renderer's shaders are built"
#endif

using namespace avis;
using namespace avis::visualizer::rendering;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Equals;

namespace
{
    // 12a §2's example
    constexpr const char* format_1_example = R"(avis_shader_manifest: 1
shaders:
  - id: lighting.main.cs
    file: lighting.main.cs.cso
    stage: cs
    entry: main
    defines: []
    xxh3: "5f0c1e2a9b3d7a41"
  - id: gbuffer.vertex_main.alpha_mask.vs
    file: gbuffer.vertex_main.alpha_mask.vs.cso
    stage: vs
    entry: vertex_main
    defines: [ALPHA_MASK]
    xxh3: "0b9e44d1c37a8f02"
)";

    // As dt shader-manifest writes it (18 §6)
    constexpr const char* format_2_example = R"(# Written by `dt shader-manifest` during the build; do not edit.
avis_shader_manifest: 2
shaders:
- id: lighting.main.cs
  file: lighting.main.cs.cso
  stage: cs
  entry: main
  defines: []
  xxh3: 8e4016d2ad80d9e1
  bindings:
    constant_buffers:
    - {register: 0, space: 0, size: 64}
    buffers:
    - {kind: shader_resource, register: 3, space: 1}
    - {kind: unordered_access, register: 0, space: 0}
    samplers:
    - {register: 1, space: 0}
- id: blit.pixel_main.ps
  file: blit.pixel_main.ps.cso
  stage: ps
  entry: pixel_main
  defines: [TINTED]
  xxh3: 00000000000000FF
  bindings:
    constant_buffers: []
    buffers: []
    samplers: []
)";

    // One format 2 entry with `replace` substituted for `original` in its text
    std::string entry_with(const std::string& original, const std::string& replacement)
    {
        std::string text = R"(avis_shader_manifest: 2
shaders:
- id: a.main.cs
  file: a.main.cs.cso
  stage: cs
  entry: main
  defines: []
  xxh3: 0123456789abcdef
  bindings:
    constant_buffers: [{register: 0, space: 0, size: 16}]
    buffers: [{kind: shader_resource, register: 0, space: 0}]
    samplers: [{register: 0, space: 0}]
)";
        const auto position = text.find(original);
        REQUIRE(position != std::string::npos);
        text.replace(position, original.size(), replacement);
        return text;
    }

    std::string error_of(const std::string& text)
    {
        CAPTURE(text);
        const auto manifest = parse_shader_manifest(text, "manifest.yaml");
        REQUIRE_FALSE(manifest.has_value());
        CHECK(manifest.error().domain == error_domain::parse);
        return manifest.error().message;
    }

    std::string read_text(const std::filesystem::path& path)
    {
        std::ifstream file{ path, std::ios::binary };
        REQUIRE(file.is_open());
        return { std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
    }
} // namespace

TEST_CASE("the manifest parser reads format 1", "[renderer][unit]")
{
    const auto manifest = parse_shader_manifest(format_1_example);
    REQUIRE(manifest.has_value());
    REQUIRE(manifest->entries.size() == 2);

    const graphics::shader_manifest_entry& lighting = manifest->entries[0];
    CHECK(lighting.id == "lighting.main.cs");
    CHECK(lighting.blob == "lighting.main.cs.cso");
    CHECK(lighting.stage == graphics::shader_stage::compute);
    CHECK(lighting.entry == "main");
    CHECK(lighting.defines.empty());
    CHECK(lighting.content_hash == 0x5f0c1e2a9b3d7a41);
    CHECK_FALSE(lighting.bindings.has_value());

    const graphics::shader_manifest_entry& gbuffer = manifest->entries[1];
    CHECK(gbuffer.id == "gbuffer.vertex_main.alpha_mask.vs");
    CHECK(gbuffer.stage == graphics::shader_stage::vertex);
    CHECK(gbuffer.entry == "vertex_main");
    CHECK(gbuffer.defines == std::vector<std::string>{ "ALPHA_MASK" });
    CHECK(gbuffer.content_hash == 0x0b9e44d1c37a8f02);
}

TEST_CASE("the manifest parser reads format 2 with bindings", "[renderer][unit]")
{
    const auto manifest = parse_shader_manifest(format_2_example);
    REQUIRE(manifest.has_value());
    REQUIRE(manifest->entries.size() == 2);

    const graphics::shader_manifest_entry& lighting = manifest->entries[0];
    CHECK(lighting.content_hash == 0x8e4016d2ad80d9e1);
    REQUIRE(lighting.bindings.has_value());
    CHECK(
        *lighting.bindings ==
        graphics::shader_bindings{
            .constant_buffers = { { .shader_register = 0, .register_space = 0, .size = 64 } },
            .buffers = { { .kind = graphics::buffer_binding_kind::shader_resource,
                           .shader_register = 3,
                           .register_space = 1 },
                         { .kind = graphics::buffer_binding_kind::unordered_access, .shader_register = 0 } },
            .samplers = { { .shader_register = 1 } },
        });

    const graphics::shader_manifest_entry& blit = manifest->entries[1];
    CHECK(blit.stage == graphics::shader_stage::pixel);
    CHECK(blit.content_hash == 0xff); // upper-case digits
    REQUIRE(blit.bindings.has_value());
    CHECK(*blit.bindings == graphics::shader_bindings{});
}

TEST_CASE("the manifest parser accepts an empty shader list and optional keys", "[renderer][unit]")
{
    const auto empty = parse_shader_manifest("avis_shader_manifest: 2\nshaders: []\n");
    REQUIRE(empty.has_value());
    CHECK(empty->entries.empty());

    // entry and defines are informational and optional
    const auto minimal = parse_shader_manifest(entry_with("  entry: main\n  defines: []\n", ""));
    REQUIRE(minimal.has_value());
    CHECK(minimal->entries.at(0).entry.empty());
    CHECK(minimal->entries.at(0).defines.empty());
}

TEST_CASE("the manifest parser reports the position and key path of each error", "[renderer][unit]")
{
    SECTION("the version")
    {
        CHECK_THAT(error_of("shaders: []\n"), Equals("manifest.yaml:1:1: avis_shader_manifest: missing required key"));
        CHECK_THAT(
            error_of("avis_shader_manifest: 3\nshaders: []\n"),
            Equals("manifest.yaml:1:23: avis_shader_manifest: unsupported format 3 (this Visualizer reads 1 to 2)"));
        CHECK_THAT(error_of("avis_shader_manifest: 0\nshaders: []\n"), ContainsSubstring("unsupported format 0"));
        CHECK_THAT(
            error_of("avis_shader_manifest: two\nshaders: []\n"),
            ContainsSubstring("avis_shader_manifest: expected an unsigned integer, got 'two'"));
    }

    SECTION("the top level")
    {
        CHECK_THAT(error_of("- 1\n- 2\n"), ContainsSubstring("expected a mapping, got a sequence"));
        CHECK_THAT(error_of(""), Equals("manifest.yaml:0:0: expected a mapping, got null"));
        CHECK_THAT(error_of("avis_shader_manifest: 2\n"), Equals("manifest.yaml:1:1: shaders: missing required key"));
        CHECK_THAT(
            error_of("avis_shader_manifest: 2\nshaders: {}\n"),
            Equals("manifest.yaml:2:10: shaders: expected a sequence, got a mapping"));
        CHECK_THAT(
            error_of("avis_shader_manifest: 2\nshaders: [1]\n"), ContainsSubstring("shaders[0]: expected a mapping"));
    }

    SECTION("missing required keys")
    {
        CHECK_THAT(
            error_of(entry_with("- id: a.main.cs\n  file", "- file")),
            Equals("manifest.yaml:3:3: shaders[0].id: missing required key"));
        CHECK_THAT(
            error_of(entry_with("  file: a.main.cs.cso\n", "")),
            ContainsSubstring("shaders[0].file: missing required key"));
        CHECK_THAT(
            error_of(entry_with("  stage: cs\n", "")), ContainsSubstring("shaders[0].stage: missing required key"));
        CHECK_THAT(
            error_of(entry_with("  xxh3: 0123456789abcdef\n", "")),
            ContainsSubstring("shaders[0].xxh3: missing required key"));
    }

    SECTION("wrong types")
    {
        CHECK_THAT(
            error_of(entry_with("id: a.main.cs", "id: { a: 1 }")),
            ContainsSubstring("shaders[0].id: expected a string, got a mapping"));
        CHECK_THAT(error_of(entry_with("id: a.main.cs", "id: ''")), ContainsSubstring("shaders[0].id: empty id"));
        CHECK_THAT(
            error_of(entry_with("defines: []", "defines: ALPHA")),
            ContainsSubstring("shaders[0].defines: expected a sequence, got a scalar"));
        CHECK_THAT(
            error_of(entry_with("defines: []", "defines: [[A]]")),
            ContainsSubstring("shaders[0].defines[0]: expected a string, got a sequence"));
        CHECK_THAT(
            error_of(entry_with("entry: main", "entry: [main]")),
            ContainsSubstring("shaders[0].entry: expected a string"));
    }

    SECTION("a bad stage")
    {
        CHECK_THAT(
            error_of(entry_with("stage: cs", "stage: gs")),
            Equals("manifest.yaml:5:10: shaders[0].stage: expected vs, ps or cs, got 'gs'"));
    }

    SECTION("bad hashes")
    {
        for (const char* hash : { "0123456789abcde", "0123456789abcdef0", "0123456789abcdeg", "0x23456789abcdef", "" })
        {
            CAPTURE(hash);
            CHECK_THAT(
                error_of(entry_with("xxh3: 0123456789abcdef", std::string{ "xxh3: '" } + hash + "'")),
                ContainsSubstring("shaders[0].xxh3: expected 16 hexadecimal digits"));
        }
    }

    SECTION("blob paths stay inside the shader directory")
    {
        for (const char* path : { "/a.cso", "C:/shaders/a.cso", "C:a.cso", "../a.cso", "sub/../../a.cso" })
        {
            CAPTURE(path);
            CHECK_THAT(
                error_of(entry_with("file: a.main.cs.cso", std::string{ "file: '" } + path + "'")),
                ContainsSubstring("shaders[0].file: '"));
        }
        CHECK_THAT(
            error_of(entry_with("file: a.main.cs.cso", "file: ''")), ContainsSubstring("shaders[0].file: empty path"));

        const auto nested = parse_shader_manifest(entry_with("file: a.main.cs.cso", "file: sub/a.main.cs.cso"));
        REQUIRE(nested.has_value());
        CHECK(nested->entries.at(0).blob == std::filesystem::path{ "sub/a.main.cs.cso" });
    }

    SECTION("duplicate ids name both lines")
    {
        const std::string text = std::string{ format_1_example } + R"(  - id: lighting.main.cs
    file: other.cso
    stage: cs
    xxh3: "5f0c1e2a9b3d7a41"
)";
        CHECK_THAT(
            error_of(text),
            Equals("manifest.yaml:15:9: shaders[2].id: duplicate id 'lighting.main.cs', first declared on line 3"));
    }

    SECTION("unknown keys are errors (12a Q1)")
    {
        CHECK_THAT(
            error_of(std::string{ format_2_example } + "extra: 1\n"), Equals("manifest.yaml:28:1: extra: unknown key"));
        CHECK_THAT(
            error_of(entry_with("  entry: main\n", "  entry: main\n  source: a.hlsl\n")),
            Equals("manifest.yaml:7:3: shaders[0].source: unknown key"));
        CHECK_THAT(
            error_of(entry_with("    samplers:", "    textures: []\n    samplers:")),
            ContainsSubstring("shaders[0].bindings.textures: unknown key"));
        CHECK_THAT(
            error_of(entry_with("{register: 0, space: 0}]", "{register: 0, space: 0, size: 4}]")),
            ContainsSubstring("shaders[0].bindings.samplers[0].size: unknown key"));
    }

    SECTION("bindings belong to format 2, where they're required")
    {
        CHECK_THAT(
            error_of(entry_with("avis_shader_manifest: 2", "avis_shader_manifest: 1")),
            ContainsSubstring("shaders[0].bindings: unknown key"));

        const std::string without_bindings = R"(avis_shader_manifest: 2
shaders:
- {id: a.main.cs, file: a.main.cs.cso, stage: cs, xxh3: 0123456789abcdef}
)";
        CHECK_THAT(error_of(without_bindings), Equals("manifest.yaml:3:3: shaders[0].bindings: missing required key"));
    }

    SECTION("bad bindings")
    {
        CHECK_THAT(
            error_of(entry_with("    buffers: [{kind: shader_resource, register: 0, space: 0}]\n", "")),
            ContainsSubstring("shaders[0].bindings.buffers: missing required key"));
        CHECK_THAT(
            error_of(entry_with("kind: shader_resource", "kind: texture")),
            ContainsSubstring(
                "shaders[0].bindings.buffers[0].kind: expected shader_resource or unordered_access, got 'texture'"));
        CHECK_THAT(
            error_of(entry_with("size: 16", "size: -16")),
            ContainsSubstring("shaders[0].bindings.constant_buffers[0].size: expected an unsigned integer, got '-16'"));
        CHECK_THAT(
            error_of(entry_with("size: 16", "size: 4294967296")),
            ContainsSubstring("shaders[0].bindings.constant_buffers[0].size: 4294967296 is out of range"));
        CHECK_THAT(
            error_of(entry_with("{register: 0, space: 0}]", "{register: 0}]")),
            ContainsSubstring("shaders[0].bindings.samplers[0].space: missing required key"));
        CHECK_THAT(
            error_of(
                entry_with("    constant_buffers: [{register: 0, space: 0, size: 16}]", "    constant_buffers: 1")),
            ContainsSubstring("shaders[0].bindings.constant_buffers: expected a sequence"));
    }

    SECTION("malformed YAML reports yaml-cpp's position")
    {
        CHECK_THAT(error_of("avis_shader_manifest: 2\nshaders: [\n"), ContainsSubstring("manifest.yaml:"));
        const std::string message = error_of("avis_shader_manifest: [1\n");
        CHECK_THAT(message, ContainsSubstring("manifest.yaml:"));
        CHECK_FALSE(message.starts_with("manifest.yaml:0:0"));
    }

    SECTION("without a source name the message starts with the position")
    {
        const auto manifest = parse_shader_manifest(entry_with("stage: cs", "stage: gs"));
        REQUIRE_FALSE(manifest.has_value());
        CHECK_THAT(manifest.error().message, Equals("5:10: shaders[0].stage: expected vs, ps or cs, got 'gs'"));
    }
}

TEST_CASE("the manifest the build wrote for the renderer's shaders parses", "[renderer][unit]")
{
    const std::filesystem::path path =
        std::filesystem::path{ AVIS_TEST_RENDERER_SHADER_DIRECTORY } / shader_manifest_file_name;
    const auto manifest = parse_shader_manifest(read_text(path), path.generic_string());
    REQUIRE(manifest.has_value());

    std::set<std::string> ids;
    for (const graphics::shader_manifest_entry& entry : manifest->entries)
    {
        CAPTURE(entry.id);
        ids.insert(entry.id);
        CHECK(entry.content_hash.has_value());
        CHECK(entry.bindings.has_value()); // format 2
        CHECK(entry.blob == std::filesystem::path{ entry.id + ".cso" });
    }
    CHECK(ids == std::set<std::string>(shader_ids::all.begin(), shader_ids::all.end()));
}
