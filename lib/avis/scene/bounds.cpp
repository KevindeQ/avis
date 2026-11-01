#include "avis/scene/bounds.h"

namespace avis::scene
{
    bool aabb::empty() const noexcept
    {
        return (minimum.array() > maximum.array()).any();
    }

    void aabb::extend(const aabb& other) noexcept
    {
        if (other.empty())
        {
            return;
        }
        minimum = minimum.cwiseMin(other.minimum);
        maximum = maximum.cwiseMax(other.maximum);
    }

    void aabb::extend(const Eigen::Vector3f& point) noexcept
    {
        minimum = minimum.cwiseMin(point);
        maximum = maximum.cwiseMax(point);
    }

    aabb aabb::transformed(const Eigen::Affine3f& transform) const noexcept
    {
        if (empty())
        {
            return aabb{};
        }

        const Eigen::Vector3f centre = transform * (0.5f * (minimum + maximum));
        const Eigen::Vector3f half_size = transform.linear().cwiseAbs() * (0.5f * (maximum - minimum));
        return aabb{ .minimum = centre - half_size, .maximum = centre + half_size };
    }
} // namespace avis::scene
