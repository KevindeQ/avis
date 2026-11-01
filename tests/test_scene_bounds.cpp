// Axis-aligned bounding boxes (docs/tasks/10-scene-management.md §10.2)

#include "avis/scene/bounds.h"

#include <catch2/catch_test_macros.hpp>

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <random>

using namespace avis;

namespace
{
    // The box of the eight transformed corners, built without aabb::transformed
    scene::aabb transformed_corners(const scene::aabb& box, const Eigen::Affine3f& transform)
    {
        scene::aabb result;
        for (int corner = 0; corner < 8; ++corner)
        {
            const Eigen::Vector3f point{
                (corner & 1) != 0 ? box.maximum.x() : box.minimum.x(),
                (corner & 2) != 0 ? box.maximum.y() : box.minimum.y(),
                (corner & 4) != 0 ? box.maximum.z() : box.minimum.z(),
            };
            result.extend(transform * point);
        }
        return result;
    }

    bool same_box(const scene::aabb& a, const scene::aabb& b, const float tolerance)
    {
        return (a.minimum - b.minimum).cwiseAbs().maxCoeff() <= tolerance &&
               (a.maximum - b.maximum).cwiseAbs().maxCoeff() <= tolerance;
    }

    bool same_box(const scene::aabb& a, const scene::aabb& b)
    {
        return a.minimum == b.minimum && a.maximum == b.maximum;
    }
} // namespace

TEST_CASE("aabb::transformed is the box of the transformed corners", "[scene][unit]")
{
    std::mt19937 random{ 20261101 };
    std::uniform_real_distribution<float> coordinate{ -10.0f, 10.0f };
    std::uniform_real_distribution<float> size{ 0.0f, 5.0f };
    std::uniform_real_distribution<float> entry{ -2.0f, 2.0f };

    for (int iteration = 0; iteration < 1000; ++iteration)
    {
        scene::aabb box;
        box.minimum = { coordinate(random), coordinate(random), coordinate(random) };
        box.maximum = box.minimum + Eigen::Vector3f{ size(random), size(random), size(random) };

        // A general affine, so rotation, non-uniform scale, shear and mirroring are all covered
        Eigen::Affine3f transform = Eigen::Affine3f::Identity();
        transform.linear() << entry(random), entry(random), entry(random), //
            entry(random), entry(random), entry(random), //
            entry(random), entry(random), entry(random);
        transform.translation() = Eigen::Vector3f{ coordinate(random), coordinate(random), coordinate(random) };

        const scene::aabb actual = box.transformed(transform);
        CHECK_FALSE(actual.empty());
        CHECK(same_box(actual, transformed_corners(box, transform), 1e-4f));
    }
}

TEST_CASE("empty boxes stay empty", "[scene][unit]")
{
    const scene::aabb empty;
    CHECK(empty.empty());

    Eigen::Affine3f transform = Eigen::Affine3f::Identity();
    transform.translate(Eigen::Vector3f{ 1.0f, 2.0f, 3.0f });
    transform.rotate(Eigen::AngleAxisf{ 0.5f, Eigen::Vector3f::UnitY() });
    CHECK(empty.transformed(transform).empty());

    // Inverted on one axis only is still empty
    scene::aabb inverted;
    inverted.minimum = { 0.0f, 0.0f, 1.0f };
    inverted.maximum = { 1.0f, 1.0f, 0.0f };
    CHECK(inverted.empty());
    CHECK(inverted.transformed(transform).empty());
}

TEST_CASE("aabb::extend grows the box, and an empty box changes nothing", "[scene][unit]")
{
    scene::aabb box;
    box.extend(Eigen::Vector3f{ 1.0f, 2.0f, 3.0f });
    CHECK_FALSE(box.empty()); // a single point is a box of size zero
    CHECK(box.minimum == Eigen::Vector3f{ 1.0f, 2.0f, 3.0f });
    CHECK(box.maximum == Eigen::Vector3f{ 1.0f, 2.0f, 3.0f });

    box.extend(Eigen::Vector3f{ -1.0f, 5.0f, 0.0f });
    CHECK(box.minimum == Eigen::Vector3f{ -1.0f, 2.0f, 0.0f });
    CHECK(box.maximum == Eigen::Vector3f{ 1.0f, 5.0f, 3.0f });

    const scene::aabb before = box;
    box.extend(scene::aabb{});
    CHECK(same_box(box, before));

    // Extending an empty box by a box gives that box
    scene::aabb empty;
    empty.extend(before);
    CHECK(same_box(empty, before));

    // An inverted box counts as empty too
    scene::aabb inverted;
    inverted.minimum = { 0.0f, 0.0f, 10.0f };
    inverted.maximum = { 100.0f, 100.0f, -10.0f };
    box.extend(inverted);
    CHECK(same_box(box, before));

    scene::aabb other;
    other.minimum = { 0.0f, -4.0f, 1.0f };
    other.maximum = { 7.0f, 0.0f, 2.0f };
    box.extend(other);
    CHECK(box.minimum == Eigen::Vector3f{ -1.0f, -4.0f, 0.0f });
    CHECK(box.maximum == Eigen::Vector3f{ 7.0f, 5.0f, 3.0f });
}
