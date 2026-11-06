#include "rendering/clusters.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace avis::visualizer::rendering
{
    graphics::texture_extent cluster_tile_size(const graphics::texture_extent view_size) noexcept
    {
        return { std::max((view_size.width + gpu::cluster_count_x - 1) / gpu::cluster_count_x, 1u),
                 std::max((view_size.height + gpu::cluster_count_y - 1) / gpu::cluster_count_y, 1u) };
    }

    std::uint32_t cluster_slice(const float linear_depth) noexcept
    {
        if (!(linear_depth > gpu::cluster_near))
        {
            return 0;
        }
        const float slice = std::log(linear_depth / gpu::cluster_near) *
                            (static_cast<float>(gpu::cluster_count_z) / std::log(gpu::cluster_far / gpu::cluster_near));
        if (!(slice < static_cast<float>(gpu::cluster_count_z - 1)))
        {
            return gpu::cluster_count_z - 1;
        }
        return static_cast<std::uint32_t>(slice);
    }

    float cluster_slice_depth(const std::uint32_t slice) noexcept
    {
        return gpu::cluster_near * std::pow(
                                       gpu::cluster_far / gpu::cluster_near,
                                       static_cast<float>(slice) / static_cast<float>(gpu::cluster_count_z));
    }

    cluster_box cluster_bounds(
        const Eigen::Matrix4f& projection_inverse,
        const graphics::texture_extent view_size,
        const std::uint32_t x,
        const std::uint32_t y,
        const std::uint32_t z)
    {
        const graphics::texture_extent tile = cluster_tile_size(view_size);
        const std::uint32_t first_x = x * tile.width;
        const std::uint32_t first_y = y * tile.height;
        cluster_box box;
        if (first_x >= view_size.width || first_y >= view_size.height)
        {
            return box;
        }
        const std::uint32_t last_x = std::min(first_x + tile.width, view_size.width);
        const std::uint32_t last_y = std::min(first_y + tile.height, view_size.height);

        const auto corner = [&](const float pixel_x, const float pixel_y, const float depth)
        {
            const Eigen::Vector4f ndc{ pixel_x / static_cast<float>(view_size.width) * 2.0f - 1.0f,
                                       1.0f - pixel_y / static_cast<float>(view_size.height) * 2.0f,
                                       1.0f,
                                       1.0f };
            const Eigen::Vector4f near_point = projection_inverse * ndc;
            const Eigen::Vector3f on_ray = near_point.head<3>() / near_point.w();
            return Eigen::Vector3f{ on_ray * (depth / -on_ray.z()) };
        };
        const std::array depths{ cluster_slice_depth(z), cluster_slice_depth(z + 1) };
        const std::array<std::array<float, 2>, 4> corners{
            { { static_cast<float>(first_x), static_cast<float>(first_y) },
              { static_cast<float>(last_x), static_cast<float>(first_y) },
              { static_cast<float>(first_x), static_cast<float>(last_y) },
              { static_cast<float>(last_x), static_cast<float>(last_y) } }
        };
        for (const float depth : depths)
        {
            for (const auto& [pixel_x, pixel_y] : corners)
            {
                const Eigen::Vector3f point = corner(pixel_x, pixel_y, depth);
                box.minimum = box.minimum.cwiseMin(point);
                box.maximum = box.maximum.cwiseMax(point);
            }
        }
        return box;
    }

    bool sphere_overlaps_box(const Eigen::Vector3f& centre, const float radius, const cluster_box& box) noexcept
    {
        const Eigen::Vector3f outside = (box.minimum - centre).cwiseMax(centre - box.maximum).cwiseMax(0.0f);
        return outside.squaredNorm() <= radius * radius;
    }

    bool cone_overlaps_sphere(
        const Eigen::Vector3f& apex,
        const Eigen::Vector3f& axis,
        const float length,
        const float cos_angle,
        const float sin_angle,
        const Eigen::Vector3f& centre,
        const float radius) noexcept
    {
        const Eigen::Vector3f offset = centre - apex;
        const float along = offset.dot(axis);
        const float closest =
            cos_angle * std::sqrt(std::max(offset.squaredNorm() - along * along, 0.0f)) - along * sin_angle;
        return !(closest > radius || along > radius + length || along < -radius);
    }
} // namespace avis::visualizer::rendering
