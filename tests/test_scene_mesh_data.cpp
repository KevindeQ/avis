// Mesh data, the vertex encodings, mesh_builder, validation and bounds (docs/tasks/10-scene-management.md §10.4)

#include "helpers/recorded_assertions.h"

#include "avis/scene/assets/material.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/assets/mesh_validation.h"
#include "avis/scene/assets/texture_data.h"
#include "avis/scene/error.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Eigen/Dense>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace avis;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
    float angle_between(const Eigen::Vector3f& a, const Eigen::Vector3f& b)
    {
        return std::atan2(a.cross(b).norm(), a.dot(b));
    }

    // A valid mesh with three non-indexed and indexed primitives over random positions
    scene::mesh_data random_mesh(std::mt19937& random)
    {
        std::uniform_real_distribution<float> coordinate{ -50.0f, 50.0f };
        scene::mesh_data data;
        for (int vertex = 0; vertex < 30; ++vertex)
        {
            data.positions.emplace_back(coordinate(random), coordinate(random), coordinate(random));
        }
        data.primitives.push_back({ .index_first = 0, .index_count = 6, .vertex_first = 0, .vertex_count = 10 });
        data.primitives.push_back({ .index_first = 6, .index_count = 3, .vertex_first = 10, .vertex_count = 5 });
        data.primitives.push_back({ .vertex_first = 15, .vertex_count = 15 });
        data.indices = { 0, 1, 2, 9, 8, 7, 4, 3, 0 };
        return data;
    }

    void check_invalid(scene::mesh_data data, const std::string_view expected_text)
    {
        INFO("expected: " << expected_text);
        const result<void> outcome = scene::detail::validate_and_bound(data);
        REQUIRE_FALSE(outcome);
        CHECK(scene::is_error(outcome.error(), scene::error_kind::invalid_mesh));
        CHECK(outcome.error().message.contains(expected_text));
    }
} // namespace

TEST_CASE("octahedral encoding round-trips unit vectors", "[scene][unit]")
{
    std::vector<Eigen::Vector3f> directions{
        Eigen::Vector3f::UnitX(),
        -Eigen::Vector3f::UnitX(),
        Eigen::Vector3f::UnitY(),
        -Eigen::Vector3f::UnitY(),
        Eigen::Vector3f::UnitZ(),
        -Eigen::Vector3f::UnitZ(),
        // On the octahedron's folds: the equator and the edges of the lower half
        Eigen::Vector3f{ 1.0f, 1.0f, 0.0f },
        Eigen::Vector3f{ -1.0f, 1.0f, 0.0f },
        Eigen::Vector3f{ 1.0f, -1.0f, 0.0f },
        Eigen::Vector3f{ -1.0f, -1.0f, 0.0f },
        Eigen::Vector3f{ 1.0f, 0.0f, -1.0f },
        Eigen::Vector3f{ 0.0f, -1.0f, -1.0f },
        Eigen::Vector3f{ 1.0f, 1.0f, -1.0f },
        Eigen::Vector3f{ -1.0f, -1.0f, -1.0f },
        Eigen::Vector3f{ 1.0f, 1.0f, -1e-4f },
        Eigen::Vector3f{ -1.0f, 1.0f, 1e-4f },
        Eigen::Vector3f{ 1e-4f, 0.0f, -1.0f },
        Eigen::Vector3f{ -1e-4f, 1e-4f, -1.0f },
    };
    std::mt19937 random{ 20261101 };
    std::normal_distribution<float> axis{ 0.0f, 1.0f };
    for (int index = 0; index < 10'000; ++index)
    {
        directions.emplace_back(axis(random), axis(random), axis(random));
    }

    float worst = 0.0f;
    for (const Eigen::Vector3f& direction : directions)
    {
        const Eigen::Vector3f unit = direction.normalized();
        const Eigen::Vector3f decoded = scene::decode_octahedral(scene::encode_octahedral(unit));
        CHECK_THAT(decoded.norm(), WithinAbs(1.0, 1e-6));
        worst = std::max(worst, angle_between(unit, decoded));
    }
    CHECK(worst < 1e-3f);

    // Not normalised, or zero
    CHECK(angle_between(scene::decode_octahedral(scene::encode_octahedral({ 0.0f, 0.0f, -7.0f })),
                        -Eigen::Vector3f::UnitZ()) < 1e-3f);
    CHECK(angle_between(scene::decode_octahedral(scene::encode_octahedral(Eigen::Vector3f::Zero())),
                        Eigen::Vector3f::UnitZ()) < 1e-3f);
}

TEST_CASE("f16 encoding round-trips to f16 precision", "[scene][unit]")
{
    CHECK(scene::encode_f16(0.0f) == 0x0000);
    CHECK(scene::encode_f16(-0.0f) == 0x8000);
    CHECK(scene::encode_f16(1.0f) == 0x3C00);
    CHECK(scene::encode_f16(0.5f) == 0x3800);
    CHECK(scene::encode_f16(-2.0f) == 0xC000);
    CHECK(scene::encode_f16(1.0f / 3.0f) == 0x3555); // nearest: 0.333251953125
    CHECK(scene::encode_f16(65504.0f) == 0x7BFF); // largest finite
    CHECK(scene::encode_f16(70000.0f) == 0x7C00); // overflow
    CHECK(scene::encode_f16(std::numeric_limits<float>::infinity()) == 0x7C00);
    CHECK(scene::encode_f16(std::ldexp(1.0f, -24)) == 0x0001); // smallest subnormal
    CHECK(scene::encode_f16(std::ldexp(1.0f, -26)) == 0x0000); // below half of it

    for (const float value : { 0.0f, 1.0f, 0.5f, -2.0f, 0.25f, 1024.0f })
    {
        CHECK(scene::decode_f16(scene::encode_f16(value)) == value); // exactly representable
    }
    CHECK_THAT(scene::decode_f16(scene::encode_f16(1.0f / 3.0f)), WithinRel(1.0 / 3.0, 1.0 / 2048.0));
    CHECK(scene::decode_f16(0x3555) == 0.333251953125f);
    CHECK(std::isinf(scene::decode_f16(0x7C00)));
    CHECK(std::isnan(scene::decode_f16(scene::encode_f16(std::numeric_limits<float>::quiet_NaN()))));
    CHECK(scene::decode_f16(0x0001) == std::ldexp(1.0f, -24));

    // Round to nearest even: 1 + 2⁻¹¹ is halfway between 1 and the next half, and rounds down to the even one
    CHECK(scene::encode_f16(1.0f + std::ldexp(1.0f, -11)) == 0x3C00);
    CHECK(scene::encode_f16(1.0f + 3.0f * std::ldexp(1.0f, -11)) == 0x3C02);

    // Every half value survives a decode and an encode
    for (std::uint32_t bits = 0; bits <= 0xFFFF; ++bits)
    {
        const auto half = static_cast<std::uint16_t>(bits);
        const float value = scene::decode_f16(half);
        if (!std::isnan(value))
        {
            REQUIRE(scene::encode_f16(value) == half);
        }
    }
}

TEST_CASE("mesh_builder closes primitives over the vertices and indices added since the last one", "[scene][unit]")
{
    scene::mesh_builder builder{ scene::mesh_topology::triangles };
    builder.reserve(7, 9);
    for (int vertex = 0; vertex < 4; ++vertex)
    {
        builder.add_vertex({ static_cast<float>(vertex), 0.0f, 0.0f })
            .normal(Eigen::Vector3f::UnitZ())
            .uv({ 0.5f, 1.0f / 3.0f });
    }
    builder.add_index(0).add_index(1).add_index(2).add_index(2).add_index(3).add_index(0).end_primitive();
    for (int vertex = 0; vertex < 3; ++vertex)
    {
        builder.add_vertex({ 0.0f, static_cast<float>(vertex), 0.0f }).normal(Eigen::Vector3f::UnitX()).uv({ 0, 0 });
    }
    builder.add_index(0).add_index(1).add_index(2); // left open: build() closes it

    result<scene::mesh_data> built = std::move(builder).build();
    REQUIRE(built);
    const scene::mesh_data& data = *built;
    CHECK(data.attributes == (scene::vertex_attribute::normal | scene::vertex_attribute::uv0));
    CHECK(data.positions.size() == 7);
    CHECK(data.normals.size() == 7);
    CHECK(data.uv0.size() == 7);
    CHECK(data.tangents.empty());
    CHECK(data.color0.empty());
    CHECK(data.uv0[0] == std::array<std::uint16_t, 2>{ 0x3800, 0x3555 });
    REQUIRE(data.primitives.size() == 2);
    CHECK(data.primitives[0].index_first == 0);
    CHECK(data.primitives[0].index_count == 6);
    CHECK(data.primitives[0].vertex_first == 0);
    CHECK(data.primitives[0].vertex_count == 4);
    CHECK(data.primitives[1].index_first == 6);
    CHECK(data.primitives[1].index_count == 3);
    CHECK(data.primitives[1].vertex_first == 4);
    CHECK(data.primitives[1].vertex_count == 3);
    CHECK(data.bounds.empty()); // the cache computes them

    scene::mesh_data validated = data;
    REQUIRE(scene::detail::validate_and_bound(validated));
    CHECK(validated.primitives[1].bounds.maximum == Eigen::Vector3f{ 0.0f, 2.0f, 0.0f });
}

TEST_CASE("mesh_builder fails when a vertex lacks an attribute the first one has", "[scene][unit]")
{
    tests::recorded_assertions assertions;

    scene::mesh_builder builder{ scene::mesh_topology::points };
    builder.add_vertex({ 0, 0, 0 }).color({ 255, 0, 0, 255 }).tangent({ 1, 0, 0, -1 });
    builder.add_vertex({ 1, 0, 0 }).color({ 0, 255, 0, 255 }); // no tangent
    builder.add_vertex({ 2, 0, 0 }).color({ 0, 0, 255, 255 }).tangent({ 0, 1, 0, 1 });

    const result<scene::mesh_data> built = std::move(builder).build();
    REQUIRE_FALSE(built);
    CHECK(scene::is_error(built.error(), scene::error_kind::invalid_mesh));
#ifndef NDEBUG
    CHECK(assertions.count() == 1); // reported once, not per vertex
    CHECK(assertions.contains("attributes differ from the first vertex"));
#endif
}

TEST_CASE("mesh_builder encodes tangents with their sign, and an empty builder gives an empty mesh", "[scene][unit]")
{
    scene::mesh_builder builder{ scene::mesh_topology::lines };
    builder.add_vertex({ 0, 0, 0 }).tangent({ 0, 0, 1, -1 });
    builder.add_vertex({ 1, 0, 0 }).tangent({ 1, 0, 0, 1 });
    result<scene::mesh_data> built = std::move(builder).build();
    REQUIRE(built);
    CHECK(built->attributes == avis::flags<scene::vertex_attribute>{ scene::vertex_attribute::tangent });
    CHECK(built->tangent_signs == std::vector<std::int8_t>{ -1, 1 });
    CHECK(angle_between(scene::decode_octahedral(built->tangents[0]), Eigen::Vector3f::UnitZ()) < 1e-3f);
    REQUIRE(built->primitives.size() == 1);
    CHECK(built->primitives[0].vertex_count == 2);

    result<scene::mesh_data> empty = scene::mesh_builder{ scene::mesh_topology::triangles }.build();
    REQUIRE(empty);
    REQUIRE(empty->primitives.size() == 1);
    REQUIRE(scene::detail::validate_and_bound(*empty));
    CHECK(empty->bounds.empty());
    CHECK(empty->primitives[0].bounds.empty());
}

TEST_CASE("validate_and_bound rejects malformed meshes", "[scene][unit]")
{
    std::mt19937 random{ 7 };
    const scene::mesh_data valid = random_mesh(random);
    {
        scene::mesh_data copy = valid;
        REQUIRE(scene::detail::validate_and_bound(copy));
    }

    SECTION("an index out of its primitive's range")
    {
        scene::mesh_data data = valid;
        data.indices[7] = 5; // primitive 1 has 5 vertices
        check_invalid(data, "out of range");
    }
    SECTION("a stream of the wrong length")
    {
        scene::mesh_data data = valid;
        data.attributes = scene::vertex_attribute::color0;
        data.color0.resize(data.positions.size() - 1);
        check_invalid(data, "color0 has 29 entries for 30 positions");
    }
    SECTION("a stream without its attribute")
    {
        scene::mesh_data data = valid;
        data.normals.resize(data.positions.size());
        check_invalid(data, "normals has 30 entries but the attributes don't include it");
    }
    SECTION("tangent signs of the wrong length")
    {
        scene::mesh_data data = valid;
        data.attributes = scene::vertex_attribute::tangent;
        data.tangents.resize(data.positions.size());
        check_invalid(data, "tangent_signs has 0 entries");
    }
    SECTION("no primitives")
    {
        scene::mesh_data data = valid;
        data.primitives.clear();
        check_invalid(data, "no primitives");
    }
    SECTION("a primitive past the end of the vertices")
    {
        scene::mesh_data data = valid;
        data.primitives[2].vertex_count = 16;
        check_invalid(data, "primitive 2 has vertices [15, 31) past the 30 positions");
    }
    SECTION("a primitive past the end of the indices")
    {
        scene::mesh_data data = valid;
        data.primitives[1].index_count = 4;
        check_invalid(data, "primitive 1 has indices [6, 10) past the 9 indices");
    }
    SECTION("indices without an indexed primitive")
    {
        scene::mesh_data data = valid;
        data.primitives[0].index_count = 0;
        data.primitives[1].index_count = 0;
        check_invalid(data, "9 indices but no indexed primitive");
    }

    // A failure leaves the data unchanged
    scene::mesh_data data = valid;
    data.primitives.clear();
    REQUIRE_FALSE(scene::detail::validate_and_bound(data));
    CHECK(data.bounds.empty());
}

TEST_CASE("validate_and_bound computes the bounds of each primitive and their union", "[scene][unit]")
{
    std::mt19937 random{ 20261101 };
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        scene::mesh_data data = random_mesh(random);
        REQUIRE(scene::detail::validate_and_bound(data));

        Eigen::Vector3f union_minimum = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
        Eigen::Vector3f union_maximum = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
        for (const scene::mesh_primitive& primitive : data.primitives)
        {
            Eigen::Vector3f minimum = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
            Eigen::Vector3f maximum = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
            for (std::uint32_t vertex = primitive.vertex_first; vertex < primitive.vertex_first + primitive.vertex_count;
                 ++vertex)
            {
                minimum = minimum.cwiseMin(data.positions[vertex]);
                maximum = maximum.cwiseMax(data.positions[vertex]);
            }
            CHECK(primitive.bounds.minimum == minimum);
            CHECK(primitive.bounds.maximum == maximum);
            union_minimum = union_minimum.cwiseMin(minimum);
            union_maximum = union_maximum.cwiseMax(maximum);
        }
        CHECK(data.bounds.minimum == union_minimum);
        CHECK(data.bounds.maximum == union_maximum);
    }

    // Empty positions give empty bounds
    scene::mesh_data empty;
    empty.primitives.emplace_back();
    REQUIRE(scene::detail::validate_and_bound(empty));
    CHECK(empty.bounds.empty());
    CHECK(empty.primitives[0].bounds.empty());
}

TEST_CASE("byte_size counts the encoded streams", "[scene][unit]")
{
    scene::mesh_data data;
    data.attributes = scene::vertex_attribute::normal | scene::vertex_attribute::tangent |
                      scene::vertex_attribute::uv0 | scene::vertex_attribute::color0;
    data.positions.resize(10);
    data.normals.resize(10);
    data.tangents.resize(10);
    data.tangent_signs.resize(10);
    data.uv0.resize(10);
    data.color0.resize(10);
    data.indices.resize(24);
    data.primitives.resize(3); // not counted

    // Per vertex: position 12, normal 4, tangent 4 + 1, uv 4, colour 4 = 29; per index 4
    CHECK(data.byte_size() == 10 * 29 + 24 * 4);
    CHECK(scene::mesh_data{}.byte_size() == 0);

    scene::texture_data texture;
    texture.width = 4;
    texture.height = 4;
    texture.mips.emplace_back(64);
    texture.mips.emplace_back(16);
    texture.mips.emplace_back(4);
    CHECK(texture.byte_size() == 84);
}

TEST_CASE("asset handles are invalid by default, and descriptions have the glTF defaults", "[scene][unit]")
{
    CHECK_FALSE(scene::mesh_handle{}.valid());
    CHECK(scene::mesh_handle{ .index = 0, .generation = 1 }.valid());
    CHECK(scene::texture_handle{ .index = 1, .generation = 1 } < scene::texture_handle{ .index = 1, .generation = 2 });

    const scene::material_description material;
    CHECK(material.base_color_factor == scene::color{});
    CHECK(material.metallic_factor == 1.0f);
    CHECK(material.roughness_factor == 1.0f);
    CHECK(material.alpha_cutoff == 0.5f);
    CHECK_FALSE(material.base_color_texture.valid());
    CHECK(material.alpha == scene::alpha_mode::opaque);
}
