// The sun's cascaded shadow maps on the GPU (docs/tasks/11-renderer.md §11.12): a cube on a wall under a 45° sun
// against the analytic shadow, and a caster outside the camera's frustum. Each scene renders twice, with the sun
// casting shadows and without, and the tests compare the two hdr_color images pixel by pixel: the ratio is the lit
// fraction. On the renderer fixture, with the debug layer and GPU-based validation.

#include "helpers/renderer_fixture.h"
#include "rendering/cascades.h"
#include "rendering/view.h"

#include "avis/graphics/readback_data.h"
#include "avis/rhi/formats.h"
#include "avis/scene/components/rendering.h"

#include <Eigen/Core>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "shared_types.hlsli"

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::renderer_fixture;
using avis::tests::renderer_fixture_options;

namespace
{
    float half_to_float(const std::uint16_t half)
    {
        const std::uint32_t sign = (half & 0x8000u) << 16;
        const std::uint32_t exponent = (half >> 10) & 0x1Fu;
        const std::uint32_t mantissa = half & 0x3FFu;
        if (exponent == 0)
        {
            const float value = std::ldexp(static_cast<float>(mantissa), -24);
            return sign != 0 ? -value : value;
        }
        if (exponent == 31)
        {
            return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
        }
        return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
    }

    // One frame's captures, copied out of the readbacks: red of hdr_color, linear_depth and cascade_index per pixel
    struct frame_images
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<float> radiance;
        std::vector<float> depth;
        std::vector<std::uint8_t> cascade;

        std::size_t at(const std::uint32_t x, const std::uint32_t y) const
        {
            return static_cast<std::size_t>(y) * width + x;
        }
    };

    // Renders one frame of `view` and reads its hdr_color, linear_depth and cascade_index
    frame_images render(renderer_fixture& fixture, view_description view)
    {
        view.options.captures.clear();
        const rhi::texture_handle hdr = fixture.make_texture(rhi::format::rgba16_float, view.size);
        const rhi::texture_handle depth = fixture.make_texture(rhi::format::r32_float, view.size);
        const rhi::texture_handle cascade = fixture.make_texture(rhi::format::r8_uint, view.size);
        view.options.captures.push_back({ .resource = "hdr_color", .texture = hdr });
        view.options.captures.push_back({ .resource = "linear_depth", .texture = depth });
        view.options.captures.push_back({ .resource = "cascade_index", .texture = cascade });
        const std::vector views{ view };
        fixture.render_and_read(views);

        frame_images images{ .width = view.size.width, .height = view.size.height };
        const std::size_t count = static_cast<std::size_t>(images.width) * images.height;
        images.radiance.resize(count);
        images.depth.resize(count);
        images.cascade.resize(count);
        const graphics::readback_image hdr_image = fixture.read_subresource(hdr);
        const graphics::readback_image depth_image = fixture.read_subresource(depth);
        const graphics::readback_image cascade_image = fixture.read_subresource(cascade);
        for (std::uint32_t y = 0; y < images.height; ++y)
        {
            for (std::uint32_t x = 0; x < images.width; ++x)
            {
                std::uint16_t red = 0;
                std::memcpy(&red, hdr_image.bytes().data() + y * hdr_image.row_pitch() + x * 8, sizeof(red));
                images.radiance[images.at(x, y)] = half_to_float(red);
                std::memcpy(
                    &images.depth[images.at(x, y)],
                    depth_image.bytes().data() + y * depth_image.row_pitch() + x * 4,
                    sizeof(float));
                images.cascade[images.at(x, y)] =
                    static_cast<std::uint8_t>(cascade_image.bytes()[y * cascade_image.row_pitch() + x]);
            }
        }
        return images;
    }

    renderer_fixture_options shadow_options()
    {
        renderer_fixture_options options;
        options.settings.exposure.mode = exposure_mode::manual;
        options.settings.exposure.ev100 = 12.0f; // a 100 klx sun stays well inside FP16
        return options;
    }

    view_description steady_view(renderer_fixture& fixture)
    {
        view_description made = fixture.view();
        made.options.taa = false;
        return made;
    }

    scene::material_description white_lambertian()
    {
        scene::material_description material;
        material.base_color_factor = { 1.0f, 1.0f, 1.0f, 1.0f };
        material.metallic_factor = 0.0f;
        material.roughness_factor = 1.0f;
        return material;
    }

    // The view-space direction through the centre of pixel (x, y) of the fixture's camera (looking down −Z)
    Eigen::Vector3f pixel_direction(const view_description& view, const std::uint32_t x, const std::uint32_t y)
    {
        const float aspect = static_cast<float>(view.size.width) / static_cast<float>(view.size.height);
        const float scale = std::tan(0.5f * view.camera.vertical_field_of_view);
        const float ndc_x = (static_cast<float>(x) + 0.5f) / static_cast<float>(view.size.width) * 2.0f - 1.0f;
        const float ndc_y = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(view.size.height) * 2.0f;
        return { ndc_x * scale * aspect, ndc_y * scale, -1.0f };
    }

    // The signed distance from (x, y) to the boundary of [x0, x1] × [y0, y1]: negative inside
    float
        rectangle_distance(const float x, const float y, const float x0, const float x1, const float y0, const float y1)
    {
        const float dx = std::max(x0 - x, x - x1);
        const float dy = std::max(y0 - y, y - y1);
        const float outside = std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f));
        return outside > 0.0f ? outside : std::max(dx, dy);
    }

    void set_sun_shadows(renderer_fixture& fixture, const scene::entity sun, const bool casts)
    {
        fixture.world.registry().get<scene::components::light>(sun).casts_shadows = casts;
        fixture.publish();
    }

    constexpr float wall_depth = 4.0f;
    const Eigen::Vector3f sun_direction = Eigen::Vector3f{ 1.0f, 0.0f, -1.0f }.normalized(); // 45° onto the wall
} // namespace

TEST_CASE("a cube on a wall under a 45 degree sun casts the analytic shadow", "[renderer]")
{
    renderer_fixture fixture{ shadow_options() };
    // The wall faces the camera at 4 m; the unit cube stands on it, between z = −4 and −3
    fixture.add_lit_quad({ 0.0f, 0.0f, -wall_depth }, 100.0f, 100.0f, white_lambertian());
    fixture.add_unit_cube({ 0.0f, 0.0f, -wall_depth + 0.5f }, white_lambertian());
    const scene::entity sun = fixture.add_sun(sun_direction);
    fixture.publish();
    fixture.renderer->set_environment_override(Eigen::Vector3f::Zero()); // no ambient light

    const view_description main = steady_view(fixture);
    const frame_images shadowed = render(fixture, main);
    REQUIRE(fixture.renderer->view_cascades().size() == 1);
    const shadow_cascades cascades = fixture.renderer->view_cascades()[0];
    REQUIRE(cascades.count == fixture.settings.shadows.cascades);
    const float texel = cascades.cascades[0].texel_size;
    REQUIRE(wall_depth < cascades.cascades[0].blend_start); // the wall is all in cascade 0
    CHECK(fixture.renderer->debug_information().draw_lists[0].cascade_items[0] == 2);

    set_sun_shadows(fixture, sun, false);
    const frame_images unshadowed = render(fixture, main);
    CHECK(fixture.renderer->view_cascades()[0].count == 0);

    // The shadow of the cube on the wall: its footprint swept 1 m along +x by the top face. Pixels within two texels of
    // its edge are the filter's; pixels within three texels of the footprint are the contact, where the bias may leak.
    std::size_t inside = 0;
    std::size_t outside = 0;
    std::size_t acne = 0;
    std::size_t leaks = 0;
    for (std::uint32_t y = 0; y < main.size.height; ++y)
    {
        for (std::uint32_t x = 0; x < main.size.width; ++x)
        {
            const std::size_t index = shadowed.at(x, y);
            if (std::abs(shadowed.depth[index] - wall_depth) > 1e-3f)
            {
                continue; // the cube
            }
            CHECK(static_cast<std::uint32_t>(shadowed.cascade[index]) == 0);
            CHECK(static_cast<std::uint32_t>(unshadowed.cascade[index]) == gpu::no_cascade);
            const Eigen::Vector3f wall = pixel_direction(main, x, y) * wall_depth;
            const float edge = rectangle_distance(wall.x(), wall.y(), -0.5f, 1.5f, -0.5f, 0.5f);
            const float contact = rectangle_distance(wall.x(), wall.y(), -0.5f, 0.5f, -0.5f, 0.5f);
            if (std::abs(edge) < 2.0f * texel || contact < 3.0f * texel)
            {
                continue;
            }
            const float lit = shadowed.radiance[index] / unshadowed.radiance[index];
            if (edge < 0.0f)
            {
                ++inside;
                if (lit > 0.02f)
                {
                    ++leaks;
                    UNSCOPED_INFO("light at (" << wall.x() << ", " << wall.y() << "): " << lit);
                }
            }
            else
            {
                ++outside;
                if (lit < 0.99f)
                {
                    ++acne;
                    UNSCOPED_INFO("shadow at (" << wall.x() << ", " << wall.y() << "): " << lit);
                }
            }
        }
    }
    INFO("texel " << texel << " m, " << inside << " shadowed and " << outside << " lit pixels checked");
    CHECK(inside > 50);
    CHECK(outside > 5'000);
    CHECK(leaks == 0);
    CHECK(acne == 0);
}

TEST_CASE("a caster outside the camera's frustum but inside a cascade still casts", "[renderer]")
{
    renderer_fixture fixture{ shadow_options() };
    fixture.add_lit_quad({ 0.0f, 0.0f, -wall_depth }, 100.0f, 100.0f, white_lambertian());
    // Left of the view, 2 to 3 m in front of the wall: its shadow falls 2 to 3 m to its right, x in [−3.5, −1.5]
    fixture.add_unit_cube({ -5.0f, 0.0f, -wall_depth + 2.5f }, white_lambertian());
    const scene::entity sun = fixture.add_sun(sun_direction);
    fixture.publish();
    fixture.renderer->set_environment_override(Eigen::Vector3f::Zero());

    const view_description main = steady_view(fixture);
    const frame_images shadowed = render(fixture, main);
    const draw_list_statistics statistics = fixture.renderer->debug_information().draw_lists[0];
    CHECK(statistics.camera_items == 1); // only the wall is in view
    CHECK(statistics.cascade_items[0] == 2);

    set_sun_shadows(fixture, sun, false);
    const frame_images unshadowed = render(fixture, main);

    std::size_t checked = 0;
    for (std::uint32_t y = 0; y < main.size.height; ++y)
    {
        for (std::uint32_t x = 0; x < main.size.width; ++x)
        {
            const Eigen::Vector3f wall = pixel_direction(main, x, y) * wall_depth;
            const std::size_t index = shadowed.at(x, y);
            REQUIRE(std::abs(shadowed.depth[index] - wall_depth) < 1e-3f); // nothing but the wall is drawn
            const float lit = shadowed.radiance[index] / unshadowed.radiance[index];
            const float edge = rectangle_distance(wall.x(), wall.y(), -3.5f, -1.5f, -0.5f, 0.5f);
            if (edge < -0.1f)
            {
                ++checked;
                CHECK(lit <= 0.02f);
            }
            else if (edge > 0.1f)
            {
                CHECK(lit >= 0.99f);
            }
        }
    }
    CHECK(checked > 50);
}
