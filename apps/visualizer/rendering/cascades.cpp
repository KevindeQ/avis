#include "rendering/cascades.h"

#include <Eigen/Geometry>
#include <Eigen/LU>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace avis::visualizer::rendering
{
    namespace
    {
        gpu::float4x4 to_gpu(const Eigen::Matrix4f& matrix) noexcept
        {
            // Both are column-major (shared_types.hlsli)
            gpu::float4x4 result{};
            std::memcpy(&result, matrix.data(), sizeof(result));
            return result;
        }

        gpu::shadow_cascade_data cascade_data(const shadow_cascade& cascade) noexcept
        {
            return { .shadow_from_world = to_gpu(cascade.shadow_from_world),
                     .split_far = cascade.far_depth,
                     .blend_start = cascade.blend_start,
                     .texel_size = cascade.texel_size,
                     .padding0 = 0.0f };
        }
    } // namespace

    cascade_parameters cascade_parameters_of(const shadow_settings& settings) noexcept
    {
        return { .count = std::clamp(settings.cascades, 1u, maximum_shadow_cascades),
                 .resolution = std::max(settings.resolution, 1u),
                 .distance = settings.distance };
    }

    std::array<float, maximum_shadow_cascades + 1>
        cascade_splits(const float near, const float distance, std::uint32_t count, const float lambda) noexcept
    {
        count = std::clamp(count, 1u, maximum_shadow_cascades);
        std::array<float, maximum_shadow_cascades + 1> splits;
        splits.fill(distance);
        splits[0] = near;
        for (std::uint32_t index = 1; index < count; ++index)
        {
            const float fraction = static_cast<float>(index) / static_cast<float>(count);
            const float logarithmic = near * std::pow(distance / near, fraction);
            const float uniform = near + (distance - near) * fraction;
            splits[index] = lambda * logarithmic + (1.0f - lambda) * uniform;
        }
        splits[count] = distance;
        return splits;
    }

    slice_sphere slice_bounding_sphere(
        const float near, const float far, const float tan_half_x, const float tan_half_y) noexcept
    {
        // The corners of a depth d are k·d from the axis. The centre where the near and the far corners are equally far
        // is at (f + n)(1 + k²) / 2; past the far plane, the far cap's centre holds everything.
        const float k2 = tan_half_x * tan_half_x + tan_half_y * tan_half_y;
        const float centre = std::min(0.5f * (far + near) * (1.0f + k2), far);
        const float to_near = std::sqrt((centre - near) * (centre - near) + k2 * near * near);
        const float to_far = std::sqrt((far - centre) * (far - centre) + k2 * far * far);
        return { .centre_depth = centre, .radius = std::max(to_near, to_far) };
    }

    Eigen::Matrix3f light_rotation(const Eigen::Vector3f& direction) noexcept
    {
        Eigen::Vector3f towards_sun = -direction;
        if (!(towards_sun.squaredNorm() > 0.0f))
        {
            towards_sun = Eigen::Vector3f::UnitZ();
        }
        towards_sun.normalize();
        const Eigen::Vector3f reference =
            std::abs(towards_sun.z()) > 0.99f ? Eigen::Vector3f::UnitY() : Eigen::Vector3f::UnitZ();
        const Eigen::Vector3f x = reference.cross(towards_sun).normalized();
        const Eigen::Vector3f y = towards_sun.cross(x);
        Eigen::Matrix3f rotation;
        rotation.row(0) = x;
        rotation.row(1) = y;
        rotation.row(2) = towards_sun;
        return rotation;
    }

    shadow_cascades compute_cascades(
        const rendering::camera& camera,
        const float aspect_ratio,
        const Eigen::Vector3f& direction,
        const cascade_parameters& parameters)
    {
        shadow_cascades result;
        result.count = std::clamp(parameters.count, 1u, maximum_shadow_cascades);
        result.resolution = std::max(parameters.resolution, 1u);
        result.light_from_world = light_rotation(direction);
        result.direction = -result.light_from_world.row(2).transpose();

        const float near = camera.near_plane;
        const float distance = std::max(
            camera.far_plane ? std::min(parameters.distance, *camera.far_plane) : parameters.distance, near * 1.001f);
        const std::array splits = cascade_splits(near, distance, result.count, parameters.split_lambda);
        const float tan_half_y = std::tan(0.5f * camera.vertical_field_of_view);
        const float tan_half_x = tan_half_y * aspect_ratio;
        const Eigen::Vector3f camera_position = camera.world_from_view.translation();
        const Eigen::Vector3f camera_forward = -(camera.world_from_view.linear() * Eigen::Vector3f::UnitZ());

        for (std::uint32_t index = 0; index < result.count; ++index)
        {
            shadow_cascade& cascade = result.cascades[index];
            cascade.near_depth = splits[index];
            cascade.far_depth = splits[index + 1];
            cascade.blend_start =
                cascade.far_depth - parameters.blend_fraction * (cascade.far_depth - cascade.near_depth);

            const slice_sphere sphere =
                slice_bounding_sphere(cascade.near_depth, cascade.far_depth, tan_half_x, tan_half_y);
            cascade.radius = sphere.radius;
            cascade.texel_size = 2.0f * sphere.radius / static_cast<float>(result.resolution);

            // The centre in light space, x and y snapped to the texel grid of the world origin
            const Eigen::Vector3f centre = camera_position + camera_forward * sphere.centre_depth;
            const Eigen::Vector3f light_centre = result.light_from_world * centre;
            cascade.unsnapped_origin = light_centre.head<2>();
            cascade.origin = (cascade.unsnapped_origin / cascade.texel_size).array().round() * cascade.texel_size;

            // View: light space moved to the origin; z_v from −r (depth 0) to r + extension (depth 1)
            const float r = sphere.radius;
            const float extent = 2.0f * r + parameters.caster_extension;
            cascade.depth_near = light_centre.z() + r + parameters.caster_extension;
            cascade.depth_far = light_centre.z() - r;
            Eigen::Matrix4f view_from_world = Eigen::Matrix4f::Identity();
            view_from_world.topLeftCorner<3, 3>() = result.light_from_world;
            view_from_world.topRightCorner<3, 1>() =
                -Eigen::Vector3f{ cascade.origin.x(), cascade.origin.y(), light_centre.z() };
            Eigen::Matrix4f projection = Eigen::Matrix4f::Zero();
            projection(0, 0) = 1.0f / r;
            projection(1, 1) = 1.0f / r;
            projection(2, 2) = 1.0f / extent;
            projection(2, 3) = r / extent;
            projection(3, 3) = 1.0f;

            cascade.viewpoint = { .view_from_world = view_from_world, .projection = projection };
            cascade.shadow_from_world = projection * view_from_world;
            result.viewpoints[index] = cascade.viewpoint;
        }
        return result;
    }

    gpu::shadow_constants
        make_shadow_constants(const shadow_cascades& cascades, const std::uint32_t shadow_map) noexcept
    {
        gpu::shadow_constants constants{};
        const std::array<gpu::shadow_cascade_data*, maximum_shadow_cascades> slots{
            &constants.cascade0, &constants.cascade1, &constants.cascade2, &constants.cascade3
        };
        for (std::uint32_t index = 0; index < cascades.count; ++index)
        {
            *slots[index] = cascade_data(cascades.cascades[index]);
        }
        constants.shadow_map = shadow_map;
        constants.cascade_count = cascades.count;
        constants.resolution = static_cast<float>(cascades.resolution);
        constants.normal_offset = shadow_normal_offset_texels;
        constants.filter_radius = shadow_filter_radius_texels;
        return constants;
    }

    gpu::view_constants make_cascade_view_constants(
        const shadow_cascades& cascades, const std::uint32_t index, const std::uint32_t frame_index)
    {
        const shadow_cascade& cascade = cascades.cascades[index];
        const Eigen::Matrix4f& view = cascade.viewpoint.view_from_world;
        const Eigen::Matrix4f& projection = cascade.viewpoint.projection;
        const Eigen::Matrix4f view_inverse = view.inverse();
        const Eigen::Matrix4f view_projection = cascade.shadow_from_world;

        gpu::view_constants constants{};
        constants.view = to_gpu(view);
        constants.view_inverse = to_gpu(view_inverse);
        constants.projection = to_gpu(projection);
        constants.projection_inverse = to_gpu(projection.inverse());
        constants.projection_unjittered = constants.projection;
        constants.view_projection = to_gpu(view_projection);
        constants.view_projection_unjittered = constants.view_projection;
        constants.view_projection_previous = constants.view_projection;
        constants.view_projection_inverse = to_gpu(view_projection.inverse());
        constants.jitter = { 0.0f, 0.0f };
        const auto size = static_cast<float>(cascades.resolution);
        constants.size = { size, size };
        constants.inverse_size = { 1.0f / size, 1.0f / size };
        constants.near_plane = 0.0f;
        constants.device_pixel_ratio = 1.0f;
        // On the sun's side of the cascade: what camera-facing normals face
        const float near_z = cascade.depth_near + view(2, 3); // light-space z to view z
        const Eigen::Vector4f eye = view_inverse * Eigen::Vector4f{ 0.0f, 0.0f, near_z, 1.0f };
        constants.camera_position = { eye.x(), eye.y(), eye.z() };
        constants.frame_index = frame_index;
        return constants;
    }
} // namespace avis::visualizer::rendering
