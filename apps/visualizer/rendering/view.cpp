#include "rendering/view.h"

#include <cmath>

namespace avis::visualizer::rendering
{
    Eigen::Matrix4f camera::view_from_world() const
    {
        return world_from_view.inverse(Eigen::Affine).matrix();
    }

    Eigen::Matrix4f camera::projection(const float aspect_ratio) const
    {
        const float focal = 1.0f / std::tan(0.5f * vertical_field_of_view);
        const float near_distance = near_plane;

        // x and y as usual; w = −z. Depth = z_clip / w is near / −z for an infinite far plane, and goes from 1 at the
        // near plane to 0 at the far one with a finite far plane.
        Eigen::Matrix4f matrix = Eigen::Matrix4f::Zero();
        matrix(0, 0) = focal / aspect_ratio;
        matrix(1, 1) = focal;
        matrix(3, 2) = -1.0f;
        if (far_plane)
        {
            const float far_distance = *far_plane;
            matrix(2, 2) = near_distance / (far_distance - near_distance);
            matrix(2, 3) = near_distance * far_distance / (far_distance - near_distance);
        }
        else
        {
            matrix(2, 3) = near_distance;
        }
        return matrix;
    }

    std::string_view to_string(const debug_blit_mode mode) noexcept
    {
        switch (mode)
        {
        case debug_blit_mode::raw:
            return "raw";
        case debug_blit_mode::depth_linear:
            return "depth_linear";
        case debug_blit_mode::normal_octahedral:
            return "normal_octahedral";
        case debug_blit_mode::velocity:
            return "velocity";
        case debug_blit_mode::ao:
            return "ao";
        case debug_blit_mode::cascade_index:
            return "cascade_index";
        case debug_blit_mode::cluster_light_count:
            return "cluster_light_count";
        case debug_blit_mode::luminance_heat:
            return "luminance_heat";
        case debug_blit_mode::alpha_only:
            return "alpha_only";
        }
        return "unknown";
    }
} // namespace avis::visualizer::rendering
