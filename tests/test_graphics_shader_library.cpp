// Shader ids and the shader library (docs/tasks/08-graphics-shaders-and-pipelines.md §8.2)

#include "rhi_test_compute_shader.h"
#include "rhi_test_vertex_shader.h"

#include "avis/graphics/error.h"
#include "avis/graphics/shader_bindings.h"
#include "avis/graphics/shader_library.h"
#include "avis/graphics/shader_library_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace avis;
using namespace avis::graphics::literals;

namespace
{
    std::vector<std::byte> bytes(const std::string_view text)
    {
        std::vector<std::byte> result(text.size());
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            result[index] = static_cast<std::byte>(text[index]);
        }
        return result;
    }

    template<std::size_t Size>
    std::vector<std::byte> bytes(const unsigned char (&array)[Size])
    {
        const auto view = std::as_bytes(std::span{ array });
        return { view.begin(), view.end() };
    }

    graphics::shader_blob blob(
        std::string id,
        const graphics::shader_stage stage,
        const std::string_view bytecode,
        std::optional<graphics::shader_bindings> bindings = std::nullopt)
    {
        return { .id = std::move(id), .stage = stage, .bytecode = bytes(bytecode), .bindings = std::move(bindings) };
    }

    graphics::shader_bindings example_bindings()
    {
        return {
            .constant_buffers = { { .shader_register = 1, .register_space = 0, .size = 64 } },
            .buffers = { { .kind = graphics::buffer_binding_kind::unordered_access, .shader_register = 0 } },
            .samplers = { { .shader_register = 2 } },
        };
    }

    bool contains(const std::string& text, const std::string_view part)
    {
        return text.find(part) != std::string::npos;
    }
} // namespace

TEST_CASE("shader ids are FNV-1a hashes computed at compile time", "[graphics][unit]")
{
    // python -c "h=0xcbf29ce484222325
    // for b in b'lighting.main.cs': h=((h^b)*0x100000001b3)&0xFFFFFFFFFFFFFFFF
    // print(hex(h))"
    constexpr std::uint64_t reference = 0xe1bea9a2a9778370ULL;

    static_assert("lighting.main.cs"_shader_id == graphics::make_shader_id("lighting.main.cs"));
    static_assert("lighting.main.cs"_shader_id.hash == reference);
    static_assert("lighting.main.cs"_shader_id != "lighting.main.ps"_shader_id);
    // The empty string hashes to the offset basis
    static_assert(graphics::make_shader_id("").hash == 0xcbf29ce484222325ULL);

    const std::string runtime_text = "lighting.main.cs";
    CHECK(graphics::make_shader_id(runtime_text) == "lighting.main.cs"_shader_id);
}

TEST_CASE("shader_library::find returns the stage, the bytecode and the bindings", "[graphics][unit]")
{
    std::vector<graphics::shader_blob> blobs;
    blobs.push_back(blob("lighting.main.cs", graphics::shader_stage::compute, "compute bytes", example_bindings()));
    blobs.push_back(blob("mesh.main.vs", graphics::shader_stage::vertex, "vertex bytes"));
    blobs.push_back(blob("mesh.main.ps", graphics::shader_stage::pixel, "pixel bytes", graphics::shader_bindings{}));

    auto library = graphics::shader_library::create(std::move(blobs), "test");
    REQUIRE(library.has_value());
    CHECK(library->size() == 3);
    CHECK(library->name() == "test");

    const auto compute = library->find("lighting.main.cs"_shader_id);
    REQUIRE(compute.has_value());
    CHECK(compute->id == "lighting.main.cs");
    CHECK(compute->stage == graphics::shader_stage::compute);
    CHECK(std::vector<std::byte>(compute->bytecode.begin(), compute->bytecode.end()) == bytes("compute bytes"));
    REQUIRE(compute->bindings != nullptr);
    CHECK(*compute->bindings == example_bindings());

    const auto vertex = library->find("mesh.main.vs"_shader_id);
    REQUIRE(vertex.has_value());
    CHECK(vertex->stage == graphics::shader_stage::vertex);
    CHECK(vertex->bindings == nullptr);

    const auto pixel = library->find("mesh.main.ps"_shader_id);
    REQUIRE(pixel.has_value());
    CHECK(pixel->stage == graphics::shader_stage::pixel);
    REQUIRE(pixel->bindings != nullptr);
    CHECK(*pixel->bindings == graphics::shader_bindings{});

    CHECK_FALSE(library->find("unknown.main.cs"_shader_id).has_value());
}

TEST_CASE("an empty shader library finds nothing", "[graphics][unit]")
{
    auto library = graphics::shader_library::create({});
    REQUIRE(library.has_value());
    CHECK(library->size() == 0);
    CHECK(library->name().empty());
    CHECK_FALSE(library->find("lighting.main.cs"_shader_id).has_value());
}

TEST_CASE("shader_library::create rejects bad blobs", "[graphics][unit]")
{
    SECTION("a duplicate id")
    {
        std::vector<graphics::shader_blob> blobs;
        blobs.push_back(blob("lighting.main.cs", graphics::shader_stage::compute, "first"));
        blobs.push_back(blob("other.main.cs", graphics::shader_stage::compute, "other"));
        blobs.push_back(blob("lighting.main.cs", graphics::shader_stage::compute, "second"));

        const auto library = graphics::shader_library::create(std::move(blobs), "duplicates");
        REQUIRE_FALSE(library.has_value());
        CHECK(graphics::is_error(library.error(), graphics::error_kind::duplicate_shader_id));
        CHECK(contains(library.error().message, "'lighting.main.cs' appears twice (blobs 0 and 2)"));
        CHECK(contains(library.error().message, "shader library 'duplicates'"));
    }

    SECTION("two ids with the same hash")
    {
        std::vector<graphics::shader_blob> blobs;
        blobs.push_back(blob("first.main.cs", graphics::shader_stage::compute, "first"));
        blobs.push_back(blob("second.main.cs", graphics::shader_stage::compute, "second"));
        const std::vector<std::uint64_t> hashes{ 0x1234, 0x1234 };

        const auto library = graphics::detail::create_shader_library(std::move(blobs), "collisions", hashes);
        REQUIRE_FALSE(library.has_value());
        CHECK(graphics::is_error(library.error(), graphics::error_kind::duplicate_shader_id));
        CHECK(contains(library.error().message, "'first.main.cs' and 'second.main.cs'"));
        CHECK(contains(library.error().message, "0000000000001234"));
    }

    SECTION("an empty id")
    {
        std::vector<graphics::shader_blob> blobs;
        blobs.push_back(blob("lighting.main.cs", graphics::shader_stage::compute, "bytes"));
        blobs.push_back(blob("", graphics::shader_stage::compute, "bytes"));

        const auto library = graphics::shader_library::create(std::move(blobs));
        REQUIRE_FALSE(library.has_value());
        CHECK(graphics::is_error(library.error(), graphics::error_kind::invalid_argument));
        CHECK(contains(library.error().message, "shader blob 1 has an empty id"));
    }

    SECTION("empty bytecode")
    {
        std::vector<graphics::shader_blob> blobs;
        blobs.push_back(blob("lighting.main.cs", graphics::shader_stage::compute, ""));

        const auto library = graphics::shader_library::create(std::move(blobs));
        REQUIRE_FALSE(library.has_value());
        CHECK(graphics::is_error(library.error(), graphics::error_kind::invalid_argument));
        CHECK(contains(library.error().message, "shader 'lighting.main.cs' has empty bytecode"));
    }
}

TEST_CASE("the shader kind is read from a DXIL container", "[graphics][unit]")
{
    CHECK(graphics::detail::dxil_shader_stage(bytes(rhi_test_vertex_shader)) == graphics::shader_stage::vertex);
    CHECK(graphics::detail::dxil_shader_stage(bytes(rhi_test_compute_shader)) == graphics::shader_stage::compute);
    CHECK_FALSE(graphics::detail::dxil_shader_stage(bytes("not a container")).has_value());

    // A truncated container is rejected, not read past its end
    auto truncated = bytes(rhi_test_vertex_shader);
    truncated.resize(40);
    CHECK_FALSE(graphics::detail::dxil_shader_stage(truncated).has_value());

#ifndef NDEBUG
    std::vector<graphics::shader_blob> blobs;
    blobs.push_back(
        { .id = "mislabelled.main.ps",
          .stage = graphics::shader_stage::pixel,
          .bytecode = bytes(rhi_test_vertex_shader) });
    const auto library = graphics::shader_library::create(std::move(blobs));
    REQUIRE_FALSE(library.has_value());
    CHECK(graphics::is_error(library.error(), graphics::error_kind::invalid_argument));
    CHECK(contains(
        library.error().message,
        "shader 'mislabelled.main.ps' is declared as a pixel shader, but its DXIL is a vertex shader"));
#endif
}

TEST_CASE("shader libraries have distinct serials that survive a move", "[graphics][unit]")
{
    std::vector<graphics::shader_blob> first_blobs;
    first_blobs.push_back(blob("lighting.main.cs", graphics::shader_stage::compute, "bytes"));
    auto first = graphics::shader_library::create(std::move(first_blobs), "first");
    auto second = graphics::shader_library::create({}, "second");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    CHECK(first->serial() != 0);
    CHECK(second->serial() != 0);
    CHECK(first->serial() != second->serial());

    const std::uint64_t serial = first->serial();
    const auto before = first->find("lighting.main.cs"_shader_id);
    REQUIRE(before.has_value());

    graphics::shader_library moved{ std::move(*first) };
    CHECK(moved.serial() == serial);
    CHECK(moved.name() == "first");
    CHECK(moved.size() == 1);
    // The bytecode doesn't move: views taken before the move stay valid
    const auto after = moved.find("lighting.main.cs"_shader_id);
    REQUIRE(after.has_value());
    CHECK(after->bytecode.data() == before->bytecode.data());

    // NOLINTBEGIN(bugprone-use-after-move): a moved-from library is specified to be empty
    CHECK(first->size() == 0);
    CHECK(first->name().empty());
    CHECK(first->serial() == 0);
    CHECK_FALSE(first->find("lighting.main.cs"_shader_id).has_value());
    // NOLINTEND(bugprone-use-after-move)

    graphics::shader_library assigned = std::move(*second);
    assigned = std::move(moved);
    CHECK(assigned.serial() == serial);
    CHECK(assigned.size() == 1);
}
