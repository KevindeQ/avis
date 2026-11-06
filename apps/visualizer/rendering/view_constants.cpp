#include "rendering/view_constants.h"

#include <Eigen/LU>

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

        float aspect_ratio(const view_description& view) noexcept
        {
            return static_cast<float>(view.size.width) / static_cast<float>(view.size.height);
        }
    } // namespace

    float halton(std::uint32_t index, const std::uint32_t base) noexcept
    {
        float result = 0.0f;
        float fraction = 1.0f;
        while (index > 0)
        {
            fraction /= static_cast<float>(base);
            result += fraction * static_cast<float>(index % base);
            index /= base;
        }
        return result;
    }

    Eigen::Vector2f jitter_pixels(const std::uint64_t sequence) noexcept
    {
        // Element 0 is (0, 0): start at 1
        const auto index = static_cast<std::uint32_t>(sequence % jitter_sample_count) + 1;
        return { halton(index, 2) - 0.5f, halton(index, 3) - 0.5f };
    }

    Eigen::Matrix4f jitter_projection(
        const Eigen::Matrix4f& projection,
        const Eigen::Vector2f& jitter,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        // An NDC offset o moves clip x by o·w: add o times the w row. NDC y points up, pixel y down.
        const float offset_x = 2.0f * jitter.x() / static_cast<float>(width);
        const float offset_y = -2.0f * jitter.y() / static_cast<float>(height);
        Eigen::Matrix4f jittered = projection;
        jittered.row(0) += offset_x * projection.row(3);
        jittered.row(1) += offset_y * projection.row(3);
        return jittered;
    }

    Eigen::Matrix4f unjittered_view_projection(const view_description& view)
    {
        return view.camera.projection(aspect_ratio(view)) * view.camera.view_from_world();
    }

    gpu::view_constants make_view_constants(const view_frame& frame)
    {
        const view_description& view = frame.view;
        const Eigen::Matrix4f view_from_world = view.camera.view_from_world();
        const Eigen::Matrix4f world_from_view = view.camera.world_from_view.matrix();
        const Eigen::Matrix4f projection_unjittered = view.camera.projection(aspect_ratio(view));
        const Eigen::Matrix4f projection =
            jitter_projection(projection_unjittered, frame.jitter, view.size.width, view.size.height);
        const Eigen::Matrix4f view_projection = projection * view_from_world;

        gpu::view_constants constants{};
        constants.view = to_gpu(view_from_world);
        constants.view_inverse = to_gpu(world_from_view);
        constants.projection = to_gpu(projection);
        constants.projection_inverse = to_gpu(projection.inverse());
        constants.projection_unjittered = to_gpu(projection_unjittered);
        constants.view_projection = to_gpu(view_projection);
        constants.view_projection_unjittered = to_gpu(projection_unjittered * view_from_world);
        constants.view_projection_previous = to_gpu(frame.view_projection_previous);
        constants.view_projection_inverse = to_gpu(view_projection.inverse());
        constants.jitter = { 2.0f * frame.jitter.x() / static_cast<float>(view.size.width),
                             -2.0f * frame.jitter.y() / static_cast<float>(view.size.height) };
        constants.size = { static_cast<float>(view.size.width), static_cast<float>(view.size.height) };
        constants.inverse_size = { 1.0f / constants.size.x, 1.0f / constants.size.y };
        constants.near_plane = view.camera.near_plane;
        constants.device_pixel_ratio = view.device_pixel_ratio;
        const Eigen::Vector3f position = view.camera.world_from_view.translation();
        constants.camera_position = { position.x(), position.y(), position.z() };
        constants.frame_index = frame.frame_index;
        return constants;
    }
} // namespace avis::visualizer::rendering
