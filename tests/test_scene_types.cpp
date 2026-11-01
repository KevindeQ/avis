// Scene common types: ids, colours, TRS transforms, node handles (docs/tasks/10-scene-management.md §10.2)

#include "avis/scene/types.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>

using namespace avis;
using Catch::Matchers::WithinAbs;

namespace
{
    scene::transform random_transform(std::mt19937& random)
    {
        std::uniform_real_distribution<float> position{ -100.0f, 100.0f };
        std::uniform_real_distribution<float> scale{ 0.2f, 5.0f };
        std::normal_distribution<float> axis{ 0.0f, 1.0f };

        Eigen::Quaternionf rotation{ axis(random), axis(random), axis(random), axis(random) };
        rotation.normalize();
        return scene::transform{
            .position = { position(random), position(random), position(random) },
            .rotation = rotation,
            .scale = { scale(random), scale(random), scale(random) },
        };
    }

    // Frobenius distance between the linear parts of an affine and a TRS
    float linear_residual(const Eigen::Affine3f& affine, const scene::transform& trs)
    {
        return (affine.linear() - trs.to_affine().linear()).norm();
    }

    bool all_finite(const scene::transform& trs)
    {
        return trs.position.allFinite() && trs.rotation.coeffs().allFinite() && trs.scale.allFinite();
    }
} // namespace

TEST_CASE("to_affine composes Translation · Rotation · Scaling", "[scene][unit]")
{
    const float angle = std::numbers::pi_v<float> / 6.0f; // 30°
    const scene::transform trs{
        .position = { 1.0f, 2.0f, 3.0f },
        .rotation = Eigen::Quaternionf{ Eigen::AngleAxisf{ angle, Eigen::Vector3f::UnitZ() } },
        .scale = { 2.0f, 3.0f, 4.0f },
    };

    // By hand: R · diag(s) in the linear part, the position as the translation
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    Eigen::Matrix4f expected;
    expected << 2.0f * c, -3.0f * s, 0.0f, 1.0f, //
        2.0f * s, 3.0f * c, 0.0f, 2.0f, //
        0.0f, 0.0f, 4.0f, 3.0f, //
        0.0f, 0.0f, 0.0f, 1.0f;

    const Eigen::Matrix4f actual = trs.to_affine().matrix();
    CHECK(actual.isApprox(expected, 1e-6f));

    // A point goes through S, then R, then T
    const Eigen::Vector3f point{ 1.0f, 1.0f, 1.0f };
    const Eigen::Vector3f by_steps = trs.position + trs.rotation * trs.scale.cwiseProduct(point);
    CHECK((trs.to_affine() * point).isApprox(by_steps, 1e-6f));

    // The prototype's transform::to_matrix (08 §4.1) applied scale, rotate, translate to an Affine3f, which gives
    // S · R · T: the translation ends up scaled and rotated
    Eigen::Affine3f prototype = Eigen::Affine3f::Identity();
    prototype.scale(trs.scale);
    prototype.rotate(trs.rotation);
    prototype.translate(trs.position);
    CHECK_FALSE(prototype.matrix().isApprox(expected, 1e-3f));
}

TEST_CASE("from_affine round-trips a TRS", "[scene][unit]")
{
    std::mt19937 random{ 20261101 };
    for (int iteration = 0; iteration < 1000; ++iteration)
    {
        const scene::transform original = random_transform(random);
        const scene::transform recovered = scene::transform::from_affine(original.to_affine());

        CHECK(recovered.position.isApprox(original.position, 1e-5f));
        CHECK_THAT(recovered.rotation.norm(), WithinAbs(1.0, 1e-6));
        // Compare rotations as matrices: q and -q are the same rotation
        CHECK((recovered.rotation.toRotationMatrix() - original.rotation.toRotationMatrix()).cwiseAbs().maxCoeff() <
              1e-5f);
        CHECK((recovered.scale - original.scale).cwiseAbs().maxCoeff() < 1e-5f);
    }
}

TEST_CASE("from_affine gives the closest TRS for a sheared matrix", "[scene][unit]")
{
    Eigen::Affine3f sheared = Eigen::Affine3f::Identity();
    sheared.linear() << 1.0f, 0.5f, 0.0f, //
        0.0f, 1.0f, 0.0f, //
        0.0f, 0.0f, 2.0f;
    sheared.translation() = Eigen::Vector3f{ 4.0f, 5.0f, 6.0f };

    const scene::transform closest = scene::transform::from_affine(sheared);
    REQUIRE(all_finite(closest));
    CHECK(closest.position == sheared.translation());
    CHECK_THAT(closest.rotation.norm(), WithinAbs(1.0, 1e-6));
    CHECK((closest.scale.array() > 0.0f).all());

    const float residual = linear_residual(sheared, closest);
    CHECK(residual > 0.0f); // shear can't be represented exactly

    std::mt19937 random{ 7 };
    std::normal_distribution<float> nudge{ 0.0f, 0.05f };
    const float rotation_distance = (sheared.linear() - closest.rotation.toRotationMatrix()).norm();
    for (int candidate = 0; candidate < 500; ++candidate)
    {
        // The rotation is the polar decomposition's: no nearby rotation is closer to the linear part
        const Eigen::Quaternionf nearby_rotation = Eigen::Quaternionf{ closest.rotation.w() + nudge(random),
                                                                       closest.rotation.x() + nudge(random),
                                                                       closest.rotation.y() + nudge(random),
                                                                       closest.rotation.z() + nudge(random) }
                                                       .normalized();
        CHECK((sheared.linear() - nearby_rotation.toRotationMatrix()).norm() >= rotation_distance - 1e-5f);

        // For that rotation, no nearby scale fits the linear part better
        scene::transform nearby = closest;
        nearby.scale += Eigen::Vector3f{ nudge(random), nudge(random), nudge(random) };
        CHECK(linear_residual(sheared, nearby) >= residual - 1e-5f);
    }
}

TEST_CASE("from_affine stays finite for degenerate matrices", "[scene][unit]")
{
    Eigen::Affine3f flat = Eigen::Affine3f::Identity();
    flat.linear() = Eigen::Vector3f{ 1.0f, 1.0f, 0.0f }.asDiagonal();
    const scene::transform from_flat = scene::transform::from_affine(flat);
    CHECK(all_finite(from_flat));
    CHECK(from_flat.to_affine().matrix().isApprox(flat.matrix(), 1e-6f));

    Eigen::Affine3f zero = Eigen::Affine3f::Identity();
    zero.linear().setZero();
    const scene::transform from_zero = scene::transform::from_affine(zero);
    CHECK(all_finite(from_zero));
    CHECK(from_zero.scale.isZero());
}

TEST_CASE("from_srgb8 uses the exact sRGB transfer function", "[scene][unit]")
{
    const scene::color color = scene::color::from_srgb8({ 0, 128, 255, 255 });
    CHECK(color.red == 0.0f);
    // ((128 / 255 + 0.055) / 1.055)^2.4; a 2.2 gamma would give 0.2195
    CHECK_THAT(color.green, WithinAbs(0.2158605, 1e-6));
    CHECK_THAT(color.blue, WithinAbs(1.0, 1e-6));
    CHECK(color.alpha == 1.0f);

    // The linear segment near black: 10 / 255 / 12.92
    CHECK_THAT(scene::color::from_srgb8({ 10, 0, 0, 0 }).red, WithinAbs(0.0030353, 1e-6));

    // Alpha is linear
    CHECK_THAT(scene::color::from_srgb8({ 0, 0, 0, 128 }).alpha, WithinAbs(128.0 / 255.0, 1e-6));
    CHECK(scene::color::from_srgb8({ 0, 0, 0, 0 }).alpha == 0.0f);
}

TEST_CASE("helper ids use source 0xFF", "[scene][unit]")
{
    static_assert(scene::is_helper(scene::object_id{ 0xFF } << 56));
    CHECK(scene::is_helper((scene::object_id{ 0xFF } << 56) | 1234));

    // A Driver id with source 1 in bits 63–56
    CHECK_FALSE(scene::is_helper((scene::object_id{ 1 } << 56) | 1234));
    CHECK_FALSE(scene::is_helper(0));
    CHECK_FALSE(scene::is_helper(scene::object_id{ 0xFE } << 56));
}

TEST_CASE("node handles are invalid by default and ordered", "[scene][unit]")
{
    CHECK_FALSE(scene::node_handle{}.valid());
    CHECK(scene::node_handle{ .index = 0, .generation = 0 }.valid());

    const scene::node_handle a{ .index = 1, .generation = 0 };
    const scene::node_handle b{ .index = 1, .generation = 1 };
    const scene::node_handle c{ .index = 2, .generation = 0 };
    CHECK(a < b);
    CHECK(b < c);
    CHECK(a == scene::node_handle{ .index = 1, .generation = 0 });
    CHECK(a != b);
}
