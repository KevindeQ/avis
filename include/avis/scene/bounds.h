#ifndef AVIS_SCENE_BOUNDS_H
#define AVIS_SCENE_BOUNDS_H

// Axis-aligned bounding boxes for world bounds (docs/design/08-scene-management.md §8.3) and mesh primitive bounds
// (§5.1)

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <limits>

namespace avis::scene
{
    struct aabb
    {
        // Default-constructed boxes are empty: extending one by anything gives that thing
        Eigen::Vector3f minimum = Eigen::Vector3f::Constant(std::numeric_limits<float>::infinity());
        Eigen::Vector3f maximum = Eigen::Vector3f::Constant(-std::numeric_limits<float>::infinity());

        // True when minimum > maximum on any axis
        bool empty() const noexcept;

        // Union with other; an empty other changes nothing
        void extend(const aabb& other) noexcept;
        void extend(const Eigen::Vector3f& point) noexcept;

        // Arvo's method: the exact box of the eight transformed corners. The centre is transformed, and the half-size
        // is the absolute linear part times the half-size. An empty box stays empty.
        aabb transformed(const Eigen::Affine3f& transform) const noexcept;
    };
} // namespace avis::scene

#endif
