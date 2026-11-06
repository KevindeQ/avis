#ifndef AVIS_VISUALIZER_RENDERING_CLUSTERS_H
#define AVIS_VISUALIZER_RENDERING_CLUSTERS_H

// The light cluster grid on the CPU (docs/design/11-renderer.md §6.3, docs/tasks/11-renderer.md §11.11): the tile size
// of a view, the exponential depth slices, the view-space bounds of a cluster and the light intersection tests. They
// mirror lighting.hlsli and clusters.hlsl, which do the work on the GPU, so the tests can count a cluster's lights by
// brute force. The grid's dimensions are shared_types.hlsli's cluster_count_*.

#include "avis/graphics/texture.h"

#include <Eigen/Core>

#include <cstdint>
#include <limits>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    // Pixels per cluster tile: the view divided into cluster_count_x × cluster_count_y tiles, rounded up, so the last
    // tiles of a small view may lie past its edge
    graphics::texture_extent cluster_tile_size(graphics::texture_extent view_size) noexcept;

    // The slice of a linear depth (metres), clamped to the grid
    std::uint32_t cluster_slice(float linear_depth) noexcept;

    // The linear depth where slice `slice` starts; cluster_slice_depth(cluster_count_z) is cluster_far
    float cluster_slice_depth(std::uint32_t slice) noexcept;

    constexpr std::uint32_t cluster_index(const std::uint32_t x, const std::uint32_t y, const std::uint32_t z) noexcept
    {
        return x + gpu::cluster_count_x * (y + gpu::cluster_count_y * z);
    }

    struct cluster_box
    {
        Eigen::Vector3f minimum = Eigen::Vector3f::Constant(std::numeric_limits<float>::infinity());
        Eigen::Vector3f maximum = Eigen::Vector3f::Constant(-std::numeric_limits<float>::infinity());

        bool empty() const noexcept
        {
            return minimum.x() > maximum.x();
        }
    };

    // The view-space bounds of cluster (x, y, z) (clusters.hlsl's build_main): the box around the tile's four corner
    // rays between the slice's two depths. `projection_inverse` is the unjittered projection's inverse. Empty for a
    // tile past the view's edge.
    cluster_box cluster_bounds(
        const Eigen::Matrix4f& projection_inverse,
        graphics::texture_extent view_size,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t z);

    // Whether a sphere touches a box
    bool sphere_overlaps_box(const Eigen::Vector3f& centre, float radius, const cluster_box& box) noexcept;

    // Whether a cone (apex, unit axis, length, half angle as cos and sin) touches a sphere (Wronski 2017)
    bool cone_overlaps_sphere(
        const Eigen::Vector3f& apex,
        const Eigen::Vector3f& axis,
        float length,
        float cos_angle,
        float sin_angle,
        const Eigen::Vector3f& centre,
        float radius) noexcept;
} // namespace avis::visualizer::rendering

#endif
