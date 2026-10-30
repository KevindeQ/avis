// Loading a shader library from a manifest (docs/tasks/08-graphics-shaders-and-pipelines.md §8.3)

#include "helpers/runtime_fixture.h"
#include "helpers/temporary_directory.h"

#include "avis/error.h"
#include "avis/graphics/error.h"
#include "avis/graphics/shader_bindings.h"
#include "avis/graphics/shader_library.h"
#include "avis/graphics/shader_loading.h"
#include "avis/graphics/shader_manifest.h"
#include "avis/io/error.h"
#include "avis/io/io_context.h"
#include "avis/threading/parallel_helpers.h"
#include "avis/threading/thread_pool.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <xxhash.h>

using namespace avis;
using namespace avis::graphics::literals;
using avis::threading::sync_wait_result;

namespace
{
    std::uint64_t xxh3(const std::vector<std::byte>& bytes)
    {
        return XXH3_64bits(bytes.data(), bytes.size());
    }

    std::string utf8(const std::filesystem::path& path)
    {
        const std::u8string text = path.u8string();
        return { text.begin(), text.end() };
    }

    bool contains(const std::string& text, const std::string_view part)
    {
        return text.find(part) != std::string::npos;
    }

    bool is_cancelled(const avis::error& error)
    {
        return error.domain == error_domain::generic &&
               error.code == static_cast<std::int32_t>(generic_error_kind::cancelled);
    }

    graphics::shader_bindings compute_bindings()
    {
        return {
            .constant_buffers = { { .shader_register = 1, .register_space = 0, .size = 48 } },
            .buffers = { { .kind = graphics::buffer_binding_kind::unordered_access, .shader_register = 0 } },
            .samplers = {},
        };
    }

    // Writes one blob per entry and returns the manifest with correct hashes: a compute shader with bindings, a vertex
    // shader without, and a pixel shader in a subdirectory
    struct test_shaders
    {
        explicit test_shaders(const std::filesystem::path& directory)
        {
            std::filesystem::create_directories(directory / "pixel");
            add(directory, "lighting.main.cs", graphics::shader_stage::compute, "lighting.main.cs.cso", 1000, 1);
            add(directory, "mesh.main.vs", graphics::shader_stage::vertex, "mesh.main.vs.cso", 2000, 2);
            add(directory, "mesh.main.ps", graphics::shader_stage::pixel, "pixel/mesh.main.ps.cso", 3000, 3);
            manifest.entries[0].bindings = compute_bindings();
            manifest.entries[2].bindings = graphics::shader_bindings{};
        }

        void
            add(const std::filesystem::path& directory,
                std::string id,
                const graphics::shader_stage stage,
                const std::filesystem::path& blob,
                const std::size_t size,
                const std::uint32_t seed)
        {
            std::vector<std::byte> bytes = avis::tests::pattern(size, seed);
            avis::tests::write_file(directory / blob, bytes);
            manifest.entries.push_back(
                {
                    .id = std::move(id),
                    .stage = stage,
                    .blob = blob,
                    .entry = "main",
                    .defines = {},
                    .content_hash = xxh3(bytes),
                    .bindings = std::nullopt,
                });
            contents.push_back(std::move(bytes));
        }

        graphics::shader_manifest manifest;
        std::vector<std::vector<std::byte>> contents;
    };

    std::vector<std::byte> bytecode_of(const graphics::shader_library& library, const graphics::shader_id id)
    {
        const auto found = library.find(id);
        REQUIRE(found.has_value());
        return { found->bytecode.begin(), found->bytecode.end() };
    }
} // namespace

TEST_CASE_METHOD(
    avis::tests::runtime_fixture, "load_shader_library loads every entry of a manifest", "[graphics][unit]")
{
    avis::tests::temporary_directory directory;
    const test_shaders shaders{ directory.path() };

    const auto library = sync_wait_result(graphics::load_shader_library(shaders.manifest, directory.path()));
    REQUIRE(library.has_value());
    CHECK(library->size() == 3);
    CHECK(library->name() == utf8(directory.path()));
    CHECK(library->serial() != 0);

    const auto compute = library->find("lighting.main.cs"_shader_id);
    REQUIRE(compute.has_value());
    CHECK(compute->id == "lighting.main.cs");
    CHECK(compute->stage == graphics::shader_stage::compute);
    REQUIRE(compute->bindings != nullptr);
    CHECK(*compute->bindings == compute_bindings());
    CHECK(bytecode_of(*library, "lighting.main.cs"_shader_id) == shaders.contents[0]);

    const auto vertex = library->find("mesh.main.vs"_shader_id);
    REQUIRE(vertex.has_value());
    CHECK(vertex->stage == graphics::shader_stage::vertex);
    CHECK(vertex->bindings == nullptr);
    CHECK(bytecode_of(*library, "mesh.main.vs"_shader_id) == shaders.contents[1]);

    const auto pixel = library->find("mesh.main.ps"_shader_id);
    REQUIRE(pixel.has_value());
    CHECK(pixel->stage == graphics::shader_stage::pixel);
    REQUIRE(pixel->bindings != nullptr);
    CHECK(*pixel->bindings == graphics::shader_bindings{});
    CHECK(bytecode_of(*library, "mesh.main.ps"_shader_id) == shaders.contents[2]);
}

TEST_CASE_METHOD(avis::tests::runtime_fixture, "an empty manifest gives an empty library", "[graphics][unit]")
{
    avis::tests::temporary_directory directory;

    const auto library = sync_wait_result(graphics::load_shader_library({}, directory.path()));
    REQUIRE(library.has_value());
    CHECK(library->size() == 0);
    CHECK(library->name() == utf8(directory.path()));
}

TEST_CASE_METHOD(avis::tests::runtime_fixture, "load_shader_library checks content hashes", "[graphics][unit]")
{
    avis::tests::temporary_directory directory;
    test_shaders shaders{ directory.path() };

    SECTION("a changed byte fails with shader_hash_mismatch naming the entry")
    {
        std::vector<std::byte> changed = shaders.contents[1];
        changed[123] ^= std::byte{ 0x40 };
        avis::tests::write_file(directory.path() / "mesh.main.vs.cso", changed);

        const auto library = sync_wait_result(graphics::load_shader_library(shaders.manifest, directory.path()));
        REQUIRE_FALSE(library.has_value());
        CHECK(graphics::is_error(library.error(), graphics::error_kind::shader_hash_mismatch));
        const std::string& message = library.error().message;
        CHECK(contains(message, "shader 'mesh.main.vs'"));
        CHECK(contains(message, utf8(directory.path() / "mesh.main.vs.cso")));
        CHECK(contains(message, std::format("{:016x}", xxh3(changed))));
        CHECK(contains(message, std::format("{:016x}", *shaders.manifest.entries[1].content_hash)));
    }

    SECTION("entries without content_hash load unchecked")
    {
        const std::vector<std::byte> changed = avis::tests::pattern(500, 9);
        avis::tests::write_file(directory.path() / "mesh.main.vs.cso", changed);
        shaders.manifest.entries[1].content_hash.reset();

        const auto library = sync_wait_result(graphics::load_shader_library(shaders.manifest, directory.path()));
        REQUIRE(library.has_value());
        CHECK(bytecode_of(*library, "mesh.main.vs"_shader_id) == changed);
    }
}

TEST_CASE_METHOD(
    avis::tests::runtime_fixture, "load_shader_library reports the errors of its parts", "[graphics][unit]")
{
    avis::tests::temporary_directory directory;
    const std::filesystem::path shader_directory = directory.path() / "shaders";
    test_shaders shaders{ shader_directory };

    SECTION("a missing file gives the IO error with the path")
    {
        std::filesystem::remove(shader_directory / "pixel" / "mesh.main.ps.cso");

        const auto library = sync_wait_result(graphics::load_shader_library(shaders.manifest, shader_directory));
        REQUIRE_FALSE(library.has_value());
        CHECK(io::is_error(library.error(), io::error_kind::not_found));
        CHECK(contains(library.error().message, "mesh.main.ps.cso"));
    }

    SECTION("a blob path with a .. element fails before any read")
    {
        // The file exists, so only the path check can reject it; the missing file of the other entry is never read
        avis::tests::write_file(directory.path() / "x.cso", avis::tests::pattern(16));
        std::filesystem::remove(shader_directory / "mesh.main.vs.cso");
        shaders.manifest.entries[2].blob = "../x.cso";
        shaders.manifest.entries[2].content_hash.reset();

        const auto library = sync_wait_result(graphics::load_shader_library(shaders.manifest, shader_directory));
        REQUIRE_FALSE(library.has_value());
        CHECK(graphics::is_error(library.error(), graphics::error_kind::invalid_argument));
        CHECK(contains(library.error().message, "shader 'mesh.main.ps': blob path '../x.cso' leaves the shader"));
    }

    SECTION("an absolute blob path fails")
    {
        shaders.manifest.entries[0].blob = shader_directory / "lighting.main.cs.cso";

        const auto library = sync_wait_result(graphics::load_shader_library(shaders.manifest, shader_directory));
        REQUIRE_FALSE(library.has_value());
        CHECK(graphics::is_error(library.error(), graphics::error_kind::invalid_argument));
        CHECK(contains(library.error().message, "shader 'lighting.main.cs'"));
    }

    SECTION("shader_library::create's errors pass through")
    {
        shaders.manifest.entries[1].id = "lighting.main.cs";

        const auto library = sync_wait_result(graphics::load_shader_library(shaders.manifest, shader_directory));
        REQUIRE_FALSE(library.has_value());
        CHECK(graphics::is_error(library.error(), graphics::error_kind::duplicate_shader_id));
        CHECK(contains(library.error().message, "'lighting.main.cs' appears twice"));
    }
}

TEST_CASE_METHOD(avis::tests::runtime_fixture, "load_shader_library honours stop requests", "[graphics][unit]")
{
    avis::tests::temporary_directory directory;
    test_shaders shaders{ directory.path() };

    SECTION("stop requested before start")
    {
        stdexec::inplace_stop_source source;
        source.request_stop();
        const auto library = sync_wait_result(
            stdexec::write_env(
                graphics::load_shader_library(shaders.manifest, directory.path()),
                stdexec::prop{ stdexec::get_stop_token, source.get_token() }));
        REQUIRE_FALSE(library.has_value());
        CHECK(is_cancelled(library.error()));
    }

    SECTION("stop requested while reading: loaded completely or stopped")
    {
        // Large blobs, so the stop request often arrives while reads are outstanding
        for (std::uint32_t index = 0; index < 16; ++index)
        {
            shaders.add(
                directory.path(),
                std::format("large{}.main.cs", index),
                graphics::shader_stage::compute,
                std::format("large{}.cso", index),
                std::size_t{ 2 } << 20,
                index);
        }

        for (int iteration = 0; iteration < 10; ++iteration)
        {
            stdexec::inplace_stop_source source;
            std::thread stopper{ [&source] { source.request_stop(); } };
            const auto library = sync_wait_result(
                stdexec::write_env(
                    graphics::load_shader_library(shaders.manifest, directory.path()),
                    stdexec::prop{ stdexec::get_stop_token, source.get_token() }));
            stopper.join();

            if (library.has_value())
            {
                CHECK(library->size() == shaders.manifest.entries.size());
            }
            else
            {
                CHECK(is_cancelled(library.error()));
            }
        }
    }
}

TEST_CASE_METHOD(
    avis::tests::runtime_fixture, "load_shader_library reads through the given IO context", "[graphics][unit]")
{
    avis::tests::temporary_directory directory;
    const test_shaders shaders{ directory.path() };
    avis::threading::thread_pool pool{ { .capacity = 1 } };
    avis::io::io_context context{ pool, { .threads = 1 } };

    SECTION("a running context loads the library")
    {
        const auto library =
            sync_wait_result(graphics::load_shader_library(context, shaders.manifest, directory.path()));
        REQUIRE(library.has_value());
        CHECK(library->size() == 3);
    }

    SECTION("a stopped context stops the load, while the runtime's context is still running")
    {
        context.request_stop();
        const auto library =
            sync_wait_result(graphics::load_shader_library(context, shaders.manifest, directory.path()));
        REQUIRE_FALSE(library.has_value());
        INFO(std::format("{}", library.error()));
        CHECK(is_cancelled(library.error()));

        CHECK(sync_wait_result(graphics::load_shader_library(shaders.manifest, directory.path())).has_value());
    }
}
