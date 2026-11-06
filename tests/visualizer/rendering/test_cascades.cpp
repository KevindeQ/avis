// The shadow cascades' CPU maths (docs/tasks/11-renderer.md §11.12): the practical splits, the bounding-sphere fitting,
// the texel-snapped origins and the blend ranges. Hand-built cameras, no device.

#include "rendering/cascades.h"
#include "rendering/render_settings.h"
#include "rendering/view.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

using namespace avis::visualizer::rendering;

namespace
{
    constexpr float aspect = 16.0f / 9.0f;

    // A camera at `position` looking along `forward` (Z up)
    camera look_along(const Eigen::Vector3f& position, const Eigen::Vector3f& forward)
    {
        camera result;
        const Eigen::Vector3f z = -forward.normalized();
        const Eigen::Vector3f x = Eigen::Vector3f::UnitZ().cross(z).normalized();
        const Eigen::Vector3f y = z.cross(x);
        Eigen::Matrix3f rotation;
        rotation.col(0) = x;
        rotation.col(1) = y;
        rotation.col(2) = z;
        result.world_from_view = Eigen::Translation3f{ position } * Eigen::Affine3f{ rotation };
        return result;
    }

    const Eigen::Vector3f sun_45 = Eigen::Vector3f{ 1.0f, 0.5f, -1.0f }.normalized();

    // The 8 world corners of the camera's frustum slice between view depths near and far
    std::array<Eigen::Vector3f, 8> slice_corners(const camera& view, const float near, const float far)
    {
        const float tan_y = std::tan(0.5f * view.vertical_field_of_view);
        const float tan_x = tan_y * aspect;
        std::array<Eigen::Vector3f, 8> corners;
        std::size_t index = 0;
        for (const float depth : { near, far })
        {
            for (const float sx : { -1.0f, 1.0f })
            {
                for (const float sy : { -1.0f, 1.0f })
                {
                    corners[index++] =
                        view.world_from_view * Eigen::Vector3f{ sx * tan_x * depth, sy * tan_y * depth, -depth };
                }
            }
        }
        return corners;
    }
} // namespace

TEST_CASE("cascade splits are monotonic, end at the distance, and are uniform for lambda 0", "[renderer][unit]")
{
    const std::array practical = cascade_splits(0.1f, 200.0f, 4, 0.8f);
    CHECK(practical[0] == 0.1f);
    CHECK(practical[4] == Catch::Approx(200.0f));
    for (std::size_t index = 0; index < 4; ++index)
    {
        CHECK(practical[index] < practical[index + 1]);
    }

    const std::array uniform = cascade_splits(0.1f, 200.0f, 4, 0.0f);
    for (std::size_t index = 0; index <= 4; ++index)
    {
        CHECK(uniform[index] == Catch::Approx(0.1f + 199.9f * static_cast<float>(index) / 4.0f));
    }

    const std::array logarithmic = cascade_splits(0.1f, 200.0f, 4, 1.0f);
    CHECK(logarithmic[2] == Catch::Approx(std::sqrt(0.1f * 200.0f)));

    // Fewer cascades: the unused entries are the distance
    const std::array two = cascade_splits(0.1f, 50.0f, 2, 0.8f);
    CHECK(two[2] == Catch::Approx(50.0f));
    CHECK(two[3] == Catch::Approx(50.0f));
    CHECK(two[1] > 0.1f);
    CHECK(two[1] < 50.0f);
}

TEST_CASE("each cascade holds its frustum slice and blends over its last 10%", "[renderer][unit]")
{
    const camera view = look_along({ 3.0f, -2.0f, 1.5f }, { 0.3f, 1.0f, -0.2f });
    const cascade_parameters parameters{ .count = 4, .resolution = 1024, .distance = 120.0f };
    const shadow_cascades cascades = compute_cascades(view, aspect, sun_45, parameters);
    REQUIRE(cascades.count == 4);
    CHECK(cascades.used().size() == 4);
    CHECK(cascades.direction.isApprox(sun_45, 1e-5f));

    for (const shadow_cascade& cascade : cascades.used())
    {
        CHECK(
            cascade.blend_start == Catch::Approx(cascade.far_depth - 0.1f * (cascade.far_depth - cascade.near_depth)));
        CHECK(cascade.texel_size == Catch::Approx(2.0f * cascade.radius / 1024.0f));
        CHECK(cascade.shadow_from_world.isApprox(cascade.viewpoint.projection * cascade.viewpoint.view_from_world));
        for (const Eigen::Vector3f& corner : slice_corners(view, cascade.near_depth, cascade.far_depth))
        {
            const Eigen::Vector4f clip = cascade.shadow_from_world * corner.homogeneous();
            CHECK(std::abs(clip.x()) <= 1.0f + 1e-4f);
            CHECK(std::abs(clip.y()) <= 1.0f + 1e-4f);
            CHECK(clip.z() >= -1e-4f);
            CHECK(clip.z() <= 1.0f + 1e-4f);
        }
        // Reverse-Z: a point nearer the sun has a larger depth
        const Eigen::Vector3f centre = view.world_from_view * Eigen::Vector3f{ 0.0f, 0.0f, -cascade.far_depth };
        const float here = (cascade.shadow_from_world * centre.homogeneous()).z();
        const float towards_sun = (cascade.shadow_from_world * (centre - sun_45).homogeneous()).z();
        CHECK(towards_sun > here);
    }
    CHECK(cascades.cascades[0].near_depth == view.near_plane);
    CHECK(cascades.cascades[3].far_depth == Catch::Approx(120.0f));

    // A far plane nearer than the shadow distance ends the last cascade
    camera near_far = view;
    near_far.far_plane = 50.0f;
    CHECK(compute_cascades(near_far, aspect, sun_45, parameters).cascades[3].far_depth == Catch::Approx(50.0f));
}

TEST_CASE("cascade origins move by whole texels only", "[renderer][unit]")
{
    const cascade_parameters parameters{ .count = 4, .resolution = 2048, .distance = 200.0f };
    const Eigen::Vector3f forward{ 0.2f, 1.0f, -0.3f };
    const Eigen::Matrix3f light = light_rotation(sun_45);
    const Eigen::Vector3f light_x = light.row(0).transpose();
    const Eigen::Vector3f light_y = light.row(1).transpose();

    const shadow_cascades start =
        compute_cascades(look_along({ 10.0f, 20.0f, 2.0f }, forward), aspect, sun_45, parameters);
    for (std::uint32_t index = 0; index < start.count; ++index)
    {
        const shadow_cascade& cascade = start.cascades[index];
        const float texel = cascade.texel_size;
        INFO("cascade " << index << ", texel " << texel);

        // The origin is on the world's texel grid
        const Eigen::Vector2f texels = cascade.origin / texel;
        CHECK(std::abs(texels.x() - std::round(texels.x())) < 1e-2f);
        CHECK(std::abs(texels.y() - std::round(texels.y())) < 1e-2f);

        // Centre the camera on a texel, so that moves under half a texel round back to it
        const Eigen::Vector2f to_grid = cascade.origin - cascade.unsnapped_origin;
        const Eigen::Vector3f centred =
            Eigen::Vector3f{ 10.0f, 20.0f, 2.0f } + light_x * to_grid.x() + light_y * to_grid.y();
        const auto at = [&](const float dx, const float dy)
        {
            const Eigen::Vector3f position = centred + light_x * (dx * texel) + light_y * (dy * texel);
            return compute_cascades(look_along(position, forward), aspect, sun_45, parameters).cascades[index];
        };
        const shadow_cascade origin = at(0.0f, 0.0f);
        CHECK((origin.origin - cascade.origin).norm() < 0.01f * texel);

        // Less than a texel (under half each way, around the centre): unchanged
        for (const auto& [dx, dy] : { std::pair{ 0.4f, 0.0f }, std::pair{ -0.4f, 0.3f }, std::pair{ 0.0f, -0.45f } })
        {
            const shadow_cascade moved = at(dx, dy);
            CHECK((moved.origin - origin.origin).norm() < 0.01f * texel);
            CHECK(moved.radius == Catch::Approx(origin.radius));
        }

        // More: by whole texels
        const shadow_cascade far = at(2.6f, -1.3f);
        const Eigen::Vector2f moved_texels = (far.origin - origin.origin) / texel;
        CHECK(moved_texels.x() == Catch::Approx(3.0f).margin(0.02));
        CHECK(moved_texels.y() == Catch::Approx(-1.0f).margin(0.02));
    }

    // Turning the camera keeps the radii, so the texel sizes don't change
    const shadow_cascades turned =
        compute_cascades(look_along({ 10.0f, 20.0f, 2.0f }, { -1.0f, 0.4f, 0.1f }), aspect, sun_45, parameters);
    for (std::uint32_t index = 0; index < start.count; ++index)
    {
        CHECK(turned.cascades[index].texel_size == Catch::Approx(start.cascades[index].texel_size));
    }
}

TEST_CASE("cascade parameters come from render.shadows", "[renderer][unit]")
{
    const cascade_parameters parameters =
        cascade_parameters_of(shadow_settings{ .cascades = 2, .resolution = 512, .distance = 80.0f });
    CHECK(parameters.count == 2);
    CHECK(parameters.resolution == 512);
    CHECK(parameters.distance == 80.0f);
    CHECK(parameters.split_lambda == 0.8f);
    CHECK(parameters.blend_fraction == 0.1f);
    CHECK(cascade_parameters_of(shadow_settings{ .cascades = 9 }).count == maximum_shadow_cascades);

    // A sun straight down still has a light space
    const Eigen::Matrix3f rotation = light_rotation(-Eigen::Vector3f::UnitZ());
    CHECK((rotation * rotation.transpose()).isIdentity(1e-5f));
    CHECK(rotation.row(2).transpose().isApprox(Eigen::Vector3f::UnitZ()));
    CHECK(rotation.determinant() == Catch::Approx(1.0f));
}
