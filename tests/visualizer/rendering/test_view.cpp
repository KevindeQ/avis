// The view's camera (docs/tasks/11-renderer.md §11.2): view-space conventions and reverse-Z projections

#include "rendering/view.h"

#include <Eigen/Geometry>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace avis::visualizer::rendering;
using Catch::Approx;

namespace
{
    // Normalised device coordinates and depth of a view-space point
    Eigen::Vector3f project(const Eigen::Matrix4f& projection, const Eigen::Vector3f& view_point)
    {
        const Eigen::Vector4f clip = projection * view_point.homogeneous();
        return clip.head<3>() / clip.w();
    }
} // namespace

TEST_CASE("the camera's projection is reverse-Z", "[renderer][unit]")
{
    camera camera;
    camera.near_plane = 0.1f;

    SECTION("an infinite far plane: depth 1 at the near plane, towards 0 far away")
    {
        const Eigen::Matrix4f projection = camera.projection(16.0f / 9.0f);
        CHECK(project(projection, { 0.0f, 0.0f, -0.1f }).z() == Approx(1.0f));
        CHECK(project(projection, { 0.0f, 0.0f, -10.0f }).z() == Approx(0.01f));
        CHECK(project(projection, { 0.0f, 0.0f, -1.0e6f }).z() == Approx(0.0f).margin(1e-6));
        CHECK(project(projection, { 0.0f, 0.0f, -10.0f }).z() > project(projection, { 0.0f, 0.0f, -20.0f }).z());
    }

    SECTION("a finite far plane: depth 1 at the near plane, 0 at the far plane")
    {
        camera.far_plane = 100.0f;
        const Eigen::Matrix4f projection = camera.projection(1.0f);
        CHECK(project(projection, { 0.0f, 0.0f, -0.1f }).z() == Approx(1.0f));
        CHECK(project(projection, { 0.0f, 0.0f, -100.0f }).z() == Approx(0.0f).margin(1e-6));
    }

    SECTION("the field of view maps to the edges, scaled by the aspect ratio")
    {
        camera.vertical_field_of_view = std::numbers::pi_v<float> / 2.0f; // 90°: y = −z at the top edge
        const Eigen::Matrix4f projection = camera.projection(2.0f);
        CHECK(project(projection, { 0.0f, 5.0f, -5.0f }).y() == Approx(1.0f));
        CHECK(project(projection, { 10.0f, 0.0f, -5.0f }).x() == Approx(1.0f));
        CHECK(project(projection, { -10.0f, -5.0f, -5.0f }).x() == Approx(-1.0f));
    }
}

TEST_CASE("view_from_world inverts the camera's pose", "[renderer][unit]")
{
    camera camera;
    camera.world_from_view = Eigen::Translation3f{ 1.0f, 2.0f, 3.0f } *
                             Eigen::AngleAxisf{ std::numbers::pi_v<float> / 2.0f, Eigen::Vector3f::UnitY() };

    // A point 5 m in front of the camera (down its −Z axis) is at view-space (0, 0, −5)
    const Eigen::Vector3f in_front = camera.world_from_view * Eigen::Vector3f{ 0.0f, 0.0f, -5.0f };
    const Eigen::Vector4f view_point = camera.view_from_world() * in_front.homogeneous();
    CHECK(view_point.x() == Approx(0.0f).margin(1e-5));
    CHECK(view_point.y() == Approx(0.0f).margin(1e-5));
    CHECK(view_point.z() == Approx(-5.0f));
}
