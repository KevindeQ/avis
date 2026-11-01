// glTF import: geometry and axes, materials, decoded textures, deduplication, errors and stop requests
// (docs/tasks/10-scene-management.md §10.6). Fixtures under tests/data/gltf/.

#include "helpers/runtime_fixture.h"
#include "helpers/temporary_directory.h"

#include "avis/io/error.h"
#include "avis/runtime.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/gltf.h"
#include "avis/scene/assets/gltf_import.h" // private, via lib/: the decode hook
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/error.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

using namespace avis;

namespace
{
    const std::filesystem::path fixtures{ AVIS_TEST_GLTF_DIRECTORY };

    std::vector<std::byte> read_bytes(const std::filesystem::path& path)
    {
        std::ifstream stream{ path, std::ios::binary };
        const std::vector<char> characters{ std::istreambuf_iterator<char>{ stream }, {} };
        std::vector<std::byte> bytes(characters.size());
        std::memcpy(bytes.data(), characters.data(), characters.size());
        return bytes;
    }

    void write_text(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream stream{ path, std::ios::binary };
        stream << text;
    }

    template<typename T>
    T read_at(const std::vector<std::byte>& bytes, const std::size_t offset)
    {
        T value;
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    bool is_cancelled(const avis::error& error)
    {
        return error.domain == error_domain::generic &&
               error.code == static_cast<std::int32_t>(generic_error_kind::cancelled);
    }

    bool same_statistics(const scene::asset_statistics& left, const scene::asset_statistics& right)
    {
        return left.meshes == right.meshes && left.materials == right.materials && left.textures == right.textures &&
               left.current_geometry_bytes == right.current_geometry_bytes &&
               left.retained_geometry_bytes == right.retained_geometry_bytes;
    }

    // The runtime and a cache on its IO context. The cache goes first, so its destructor's check runs while the
    // runtime is alive.
    class gltf_fixture : public avis::tests::runtime_fixture
    {
    protected:
        result<scene::gltf_asset_set> load(const std::filesystem::path& path)
        {
            return threading::sync_wait_result(cache_.load_gltf(path));
        }

        scene::asset_cache cache_{ avis::runtime::io_context() };
    };

    // Removes the decode hook when the test ends, also on failure
    struct decode_hook_reset
    {
        ~decode_hook_reset()
        {
            scene::detail::set_image_decode_hook({});
        }
    };
} // namespace

TEST_CASE_METHOD(gltf_fixture, "glTF Box: geometry in Avis axes and the node hierarchy", "[scene][gltf]")
{
    const std::filesystem::path file =
        GENERATE(std::filesystem::path{ "Box/glTF/Box.gltf" }, std::filesystem::path{ "Box/glTF-Binary/Box.glb" });
    INFO(file.string());
    const scene::asset_statistics before = cache_.statistics();

    result<scene::gltf_asset_set> loaded = load(fixtures / file);
    REQUIRE(loaded.has_value());
    const scene::gltf_asset_set& set = *loaded;

    REQUIRE(set.meshes.size() == 1);
    REQUIRE(set.materials.size() == 1);
    CHECK(set.textures.empty());

    scene::versioned_mesh mesh = cache_.current(set.meshes[0]);
    REQUIRE(mesh.data != nullptr);
    CHECK(mesh.version == 1);
    REQUIRE(mesh.data->primitives.size() == 1);
    CHECK(mesh.data->topology == scene::mesh_topology::triangles);
    CHECK(mesh.data->positions.size() == 24);
    CHECK(mesh.data->indices.size() == 36);
    CHECK(mesh.data->primitives[0].vertex_count == 24);
    CHECK(mesh.data->primitives[0].index_count == 36);
    CHECK(static_cast<bool>(mesh.data->attributes & scene::vertex_attribute::normal));
    CHECK_FALSE(static_cast<bool>(mesh.data->attributes & scene::vertex_attribute::tangent));
    CHECK_FALSE(static_cast<bool>(mesh.data->attributes & scene::vertex_attribute::uv0));

    // The axis change, against the glTF buffer: positions are the accessor at 288, indices the view at 576.
    // Avis (x, y, z) = glTF (x, -z, y), so glTF's Y extent is along Avis Z.
    const std::vector<std::byte> buffer = read_bytes(fixtures / "Box/glTF/Box0.bin");
    for (std::size_t vertex = 0; vertex < 24; ++vertex)
    {
        const float x = read_at<float>(buffer, 288 + vertex * 12);
        const float y = read_at<float>(buffer, 288 + vertex * 12 + 4);
        const float z = read_at<float>(buffer, 288 + vertex * 12 + 8);
        const Eigen::Vector3f& position = mesh.data->positions[vertex];
        CHECK(position.x() == x);
        CHECK(position.y() == -z);
        CHECK(position.z() == y);

        const Eigen::Vector3f normal = scene::decode_octahedral(mesh.data->normals[vertex]);
        const Eigen::Vector3f expected{ read_at<float>(buffer, vertex * 12),
                                        -read_at<float>(buffer, vertex * 12 + 8),
                                        read_at<float>(buffer, vertex * 12 + 4) };
        CHECK((normal - expected).norm() < 1e-3f);
    }
    for (std::size_t index = 0; index < 36; ++index)
    {
        CHECK(mesh.data->indices[index] == read_at<std::uint16_t>(buffer, 576 + index * 2));
    }
    CHECK(mesh.data->bounds.minimum.isApprox(Eigen::Vector3f{ -0.5f, -0.5f, -0.5f }));
    CHECK(mesh.data->bounds.maximum.isApprox(Eigen::Vector3f{ 0.5f, 0.5f, 0.5f }));

    // Root (a -90° rotation about X in glTF, which R = RotX(+90°) leaves as it is) and the child with the mesh
    REQUIRE(set.nodes.size() == 2);
    const scene::gltf_node& root = set.nodes[0];
    CHECK(root.parent == -1);
    CHECK_FALSE(root.mesh.valid());
    const Eigen::Quaternionf expected_rotation{ Eigen::AngleAxisf{ -std::numbers::pi_v<float> / 2.0f,
                                                                   Eigen::Vector3f::UnitX() } };
    CHECK(root.local.rotation.angularDistance(expected_rotation) < 1e-5f);
    CHECK(root.local.position.isZero(1e-6f));
    CHECK(root.local.scale.isApprox(Eigen::Vector3f::Ones(), 1e-5f));

    const scene::gltf_node& child = set.nodes[1];
    CHECK(child.parent == 0);
    CHECK(child.mesh == set.meshes[0]);
    REQUIRE(child.materials.size() == 1);
    CHECK(child.materials[0] == set.materials[0]);

    const scene::versioned_material material = cache_.current(set.materials[0]);
    REQUIRE(material.data != nullptr);
    CHECK_THAT(material.data->base_color_factor.red, Catch::Matchers::WithinAbs(0.8, 1e-6));
    CHECK(material.data->base_color_factor.green == 0.0f);
    CHECK(material.data->metallic_factor == 0.0f);
    CHECK(material.data->debug_name == "Red");

    mesh = {}; // a snapshot would keep the version's bytes retained
    cache_.release_all(set);
    CHECK(same_statistics(cache_.statistics(), before));
}

TEST_CASE_METHOD(gltf_fixture, "glTF BoxTextured: a decoded base-colour texture", "[scene][gltf]")
{
    const std::filesystem::path file = GENERATE(
        std::filesystem::path{ "BoxTextured/glTF/BoxTextured.gltf" },
        std::filesystem::path{ "BoxTextured/glTF-Binary/BoxTextured.glb" });
    INFO(file.string());

    result<scene::gltf_asset_set> loaded = load(fixtures / file);
    REQUIRE(loaded.has_value());
    const scene::gltf_asset_set& set = *loaded;
    REQUIRE(set.textures.size() == 1);
    REQUIRE(set.materials.size() == 1);

    const scene::versioned_material material = cache_.current(set.materials[0]);
    REQUIRE(material.data != nullptr);
    CHECK(material.data->base_color_texture == set.textures[0]);
    CHECK_FALSE(material.data->normal_texture.valid());

    const scene::versioned_texture texture = cache_.current(set.textures[0]);
    REQUIRE(texture.data != nullptr);
    CHECK(texture.data->width == 256);
    CHECK(texture.data->height == 256);
    CHECK(texture.data->format == scene::texture_format::rgba8_srgb);
    CHECK(texture.data->generate_mips);
    REQUIRE(texture.data->mips.size() == 1);
    REQUIRE(texture.data->mips[0].size() == 256 * 256 * 4);

    // Reference texels, from the PNG's palette
    const auto texel = [&](const std::size_t x, const std::size_t y)
    {
        const std::byte* pixel = texture.data->mips[0].data() + (y * 256 + x) * 4;
        return std::array<std::uint8_t, 4>{ static_cast<std::uint8_t>(pixel[0]),
                                            static_cast<std::uint8_t>(pixel[1]),
                                            static_cast<std::uint8_t>(pixel[2]),
                                            static_cast<std::uint8_t>(pixel[3]) };
    };
    CHECK(texel(200, 40) == std::array<std::uint8_t, 4>{ 108, 173, 223, 255 });
    CHECK(texel(0, 0) == std::array<std::uint8_t, 4>{ 220, 220, 220, 255 });

    const scene::versioned_mesh mesh = cache_.current(set.meshes[0]);
    REQUIRE(mesh.data != nullptr);
    CHECK(static_cast<bool>(mesh.data->attributes & scene::vertex_attribute::uv0));
    CHECK(mesh.data->uv0.size() == 24);

    cache_.release_all(set);
}

TEST_CASE_METHOD(gltf_fixture, "glTF DamagedHelmet: textures, emissive, tangents and deduplication", "[scene][gltf]")
{
    const std::filesystem::path file = GENERATE(
        std::filesystem::path{ "DamagedHelmet/glTF/DamagedHelmet.gltf" },
        std::filesystem::path{ "DamagedHelmet/glTF-Binary/DamagedHelmet.glb" });
    INFO(file.string());
    const scene::asset_statistics before = cache_.statistics();

    result<scene::gltf_asset_set> first = load(fixtures / file);
    REQUIRE(first.has_value());
    REQUIRE(first->textures.size() == 5);
    REQUIRE(first->materials.size() == 1);
    REQUIRE(first->meshes.size() == 1);

    const scene::versioned_material material = cache_.current(first->materials[0]);
    REQUIRE(material.data != nullptr);
    CHECK(material.data->emissive_factor.isApprox(Eigen::Vector3f::Ones()));
    CHECK(material.data->base_color_texture.valid());
    CHECK(material.data->metallic_roughness_texture.valid());
    CHECK(material.data->normal_texture.valid());
    CHECK(material.data->occlusion_texture.valid());
    CHECK(material.data->emissive_texture.valid());
    CHECK(cache_.current(material.data->base_color_texture).data->format == scene::texture_format::rgba8_srgb);
    CHECK(cache_.current(material.data->emissive_texture).data->format == scene::texture_format::rgba8_srgb);
    CHECK(cache_.current(material.data->normal_texture).data->format == scene::texture_format::rgba8_unorm);
    CHECK(cache_.current(material.data->metallic_roughness_texture).data->format == scene::texture_format::rgba8_unorm);
    CHECK(cache_.current(material.data->occlusion_texture).data->format == scene::texture_format::rgba8_unorm);

    // The file has no TANGENT: they're generated for the normal texture, perpendicular to the normals
    scene::versioned_mesh mesh = cache_.current(first->meshes[0]);
    REQUIRE(mesh.data != nullptr);
    CHECK(static_cast<bool>(mesh.data->attributes & scene::vertex_attribute::normal));
    CHECK(static_cast<bool>(mesh.data->attributes & scene::vertex_attribute::tangent));
    CHECK(static_cast<bool>(mesh.data->attributes & scene::vertex_attribute::uv0));
    REQUIRE(mesh.data->tangents.size() == mesh.data->positions.size());
    REQUIRE(mesh.data->tangent_signs.size() == mesh.data->positions.size());
    std::size_t skewed = 0;
    for (std::size_t vertex = 0; vertex < mesh.data->positions.size(); ++vertex)
    {
        const Eigen::Vector3f normal = scene::decode_octahedral(mesh.data->normals[vertex]);
        const Eigen::Vector3f tangent = scene::decode_octahedral(mesh.data->tangents[vertex]);
        skewed += std::abs(normal.dot(tangent)) > 1e-2f ? 1 : 0;
    }
    CHECK(skewed == 0);
    mesh = {}; // a snapshot would keep the version's bytes retained

    // A second load of the unchanged file: the same handles, one more reference each
    result<scene::gltf_asset_set> second = load(fixtures / file);
    REQUIRE(second.has_value());
    CHECK(second->meshes == first->meshes);
    CHECK(second->materials == first->materials);
    CHECK(second->textures == first->textures);
    REQUIRE(second->nodes.size() == first->nodes.size());
    CHECK(second->nodes[0].mesh == first->nodes[0].mesh);

    const scene::asset_statistics loaded = cache_.statistics();
    cache_.release_all(*first);
    CHECK(same_statistics(cache_.statistics(), loaded)); // the second set's references keep everything
    CHECK(cache_.current(second->meshes[0]).data != nullptr);
    cache_.release_all(*second);
    CHECK(same_statistics(cache_.statistics(), before));

    // Once released, a load parses again and creates new assets
    result<scene::gltf_asset_set> third = load(fixtures / file);
    REQUIRE(third.has_value());
    CHECK(third->meshes != first->meshes);
    cache_.release_all(*third);
    CHECK(same_statistics(cache_.statistics(), before));
}

TEST_CASE_METHOD(gltf_fixture, "glTF deduplication sees a changed file", "[scene][gltf]")
{
    const avis::tests::temporary_directory directory;
    std::filesystem::copy_file(fixtures / "Box/glTF/Box.gltf", directory.file("Box.gltf"));
    std::filesystem::copy_file(fixtures / "Box/glTF/Box0.bin", directory.file("Box0.bin"));

    result<scene::gltf_asset_set> first = load(directory.file("Box.gltf"));
    REQUIRE(first.has_value());

    // Same JSON with a trailing newline: a different hash
    std::string text;
    {
        const std::vector<std::byte> bytes = read_bytes(directory.file("Box.gltf"));
        text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    write_text(directory.file("Box.gltf"), text + "\n");

    result<scene::gltf_asset_set> second = load(directory.file("Box.gltf"));
    REQUIRE(second.has_value());
    CHECK(second->meshes != first->meshes);

    cache_.release_all(*first);
    cache_.release_all(*second);
}

TEST_CASE_METHOD(gltf_fixture, "glTF import errors map to their kinds and leak nothing", "[scene][gltf]")
{
    const scene::asset_statistics before = cache_.statistics();

    SECTION("a missing file is the IO error, unchanged")
    {
        const result<scene::gltf_asset_set> loaded = load(fixtures / "Missing/Missing.gltf");
        REQUIRE_FALSE(loaded.has_value());
        CHECK(io::is_error(loaded.error(), io::error_kind::not_found));
    }

    SECTION("a missing external buffer is the IO error too")
    {
        const avis::tests::temporary_directory directory;
        std::filesystem::copy_file(fixtures / "Box/glTF/Box.gltf", directory.file("Box.gltf"));
        const result<scene::gltf_asset_set> loaded = load(directory.file("Box.gltf"));
        REQUIRE_FALSE(loaded.has_value());
        CHECK(io::is_error(loaded.error(), io::error_kind::not_found));
    }

    SECTION("a required unknown extension is gltf_unsupported")
    {
        const result<scene::gltf_asset_set> loaded = load(fixtures / "Invalid/RequiredUnknownExtension.gltf");
        REQUIRE_FALSE(loaded.has_value());
        CHECK(scene::is_error(loaded.error(), scene::error_kind::gltf_unsupported));
        CHECK_THAT(loaded.error().message, Catch::Matchers::ContainsSubstring("RequiredUnknownExtension.gltf"));
    }

    SECTION("a broken accessor is gltf_invalid")
    {
        const result<scene::gltf_asset_set> loaded = load(fixtures / "Invalid/BrokenAccessor.gltf");
        REQUIRE_FALSE(loaded.has_value());
        CHECK(scene::is_error(loaded.error(), scene::error_kind::gltf_invalid));
    }

    SECTION("a corrupt PNG is image_decode_failed, naming the image and the file")
    {
        const avis::tests::temporary_directory directory;
        std::filesystem::copy_file(fixtures / "BoxTextured/glTF/BoxTextured.gltf", directory.file("BoxTextured.gltf"));
        std::filesystem::copy_file(fixtures / "BoxTextured/glTF/BoxTextured0.bin", directory.file("BoxTextured0.bin"));
        std::vector<std::byte> png = read_bytes(fixtures / "BoxTextured/glTF/CesiumLogoFlat.png");
        // The signature and the IHDR chunk survive (33 bytes); the chunks after them are garbage
        png.resize(33);
        png.insert(png.end(), 64, std::byte{ 0xAB });
        {
            std::ofstream stream{ directory.file("CesiumLogoFlat.png"), std::ios::binary };
            stream.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        }

        const result<scene::gltf_asset_set> loaded = load(directory.file("BoxTextured.gltf"));
        REQUIRE_FALSE(loaded.has_value());
        CHECK(scene::is_error(loaded.error(), scene::error_kind::image_decode_failed));
        CHECK_THAT(loaded.error().message, Catch::Matchers::ContainsSubstring("CesiumLogoFlat.png"));
        CHECK_THAT(loaded.error().message, Catch::Matchers::ContainsSubstring("BoxTextured.gltf"));
    }

    SECTION("a cache without IO can't load")
    {
        scene::asset_cache without_io;
        const result<scene::gltf_asset_set> loaded =
            threading::sync_wait_result(without_io.load_gltf(fixtures / "Box/glTF/Box.gltf"));
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().domain == error_domain::generic);
        CHECK(loaded.error().code == static_cast<std::int32_t>(generic_error_kind::logic_error));
    }

    CHECK(same_statistics(cache_.statistics(), before));
}

TEST_CASE_METHOD(gltf_fixture, "glTF: a stop request while decoding completes with set_stopped", "[scene][gltf]")
{
    const scene::asset_statistics before = cache_.statistics();
    const decode_hook_reset reset;

    stdexec::inplace_stop_source source;
    scene::detail::set_image_decode_hook([&source](std::size_t) { source.request_stop(); });

    const auto outcome = threading::sync_wait(
        stdexec::write_env(
            cache_.load_gltf(fixtures / "DamagedHelmet/glTF/DamagedHelmet.gltf"),
            stdexec::prop{ stdexec::get_stop_token, source.get_token() }));
    CHECK_FALSE(outcome.has_value()); // set_stopped: sync_wait gives an empty optional

    CHECK(same_statistics(cache_.statistics(), before));

    // The next load isn't affected
    scene::detail::set_image_decode_hook({});
    result<scene::gltf_asset_set> loaded = load(fixtures / "BoxTextured/glTF/BoxTextured.gltf");
    REQUIRE(loaded.has_value());
    cache_.release_all(*loaded);
}

TEST_CASE_METHOD(gltf_fixture, "glTF: flat normals and a baked KHR_texture_transform", "[scene][gltf]")
{
    // One triangle without normals, at (0, 0, 0), (1, 0, 0), (0, 1, 0) in glTF, uvs (0, 0), (1, 0), (0, 1), and a
    // base-colour texture with offset (0.5, 0.25) and scale (2, 3)
    const avis::tests::temporary_directory directory;
    std::filesystem::copy_file(fixtures / "BoxTextured/glTF/CesiumLogoFlat.png", directory.file("texture.png"));
    write_text(
        directory.file("triangle.gltf"),
        R"({
    "asset": { "version": "2.0" },
    "extensionsUsed": [ "KHR_texture_transform" ],
    "scene": 0,
    "scenes": [ { "nodes": [ 0 ] } ],
    "nodes": [ { "mesh": 0, "translation": [ 1, 2, 3 ] } ],
    "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "material": 0 } ] } ],
    "materials": [ { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0,
        "extensions": { "KHR_texture_transform": { "offset": [ 0.5, 0.25 ], "scale": [ 2, 3 ] } } } } } ],
    "textures": [ { "source": 0 } ],
    "images": [ { "uri": "texture.png" } ],
    "accessors": [
        { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [ 0, 0, 0 ], "max": [ 1, 1, 0 ] },
        { "bufferView": 0, "byteOffset": 36, "componentType": 5126, "count": 3, "type": "VEC2" } ],
    "bufferViews": [ { "buffer": 0, "byteLength": 60 } ],
    "buffers": [ { "byteLength": 60,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/" } ]
})");

    result<scene::gltf_asset_set> loaded = load(directory.file("triangle.gltf"));
    REQUIRE(loaded.has_value());
    const scene::versioned_mesh mesh = cache_.current(loaded->meshes[0]);
    REQUIRE(mesh.data != nullptr);
    REQUIRE(mesh.data->positions.size() == 3);
    CHECK(mesh.data->primitives[0].index_count == 0); // flat normals un-index the primitive

    // Flat normal: glTF +Z is Avis -Y
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
    {
        const Eigen::Vector3f normal = scene::decode_octahedral(mesh.data->normals[vertex]);
        CHECK((normal - Eigen::Vector3f{ 0.0f, -1.0f, 0.0f }).norm() < 1e-3f);
    }

    const std::array<Eigen::Vector2f, 3> expected = { Eigen::Vector2f{ 0.5f, 0.25f },
                                                      Eigen::Vector2f{ 2.5f, 0.25f },
                                                      Eigen::Vector2f{ 0.5f, 3.25f } };
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
    {
        CHECK(scene::decode_f16(mesh.data->uv0[vertex][0]) == expected[vertex].x());
        CHECK(scene::decode_f16(mesh.data->uv0[vertex][1]) == expected[vertex].y());
    }

    // The node translation (1, 2, 3) becomes (1, -3, 2)
    REQUIRE(loaded->nodes.size() == 1);
    CHECK(loaded->nodes[0].local.position.isApprox(Eigen::Vector3f{ 1.0f, -3.0f, 2.0f }));

    cache_.release_all(*loaded);
}
