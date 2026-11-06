// Lighting, clusters, the sky and the baked environment (docs/tasks/11-renderer.md §11.11): the white furnace, a sun
// on a Lambertian plane, a point light's range, the light clusters against a CPU brute force, the BRDF LUT against a
// CPU integration, and when the environment is baked. Plus the CPU side: the sky model, the cluster grid and the light
// cut-off range. On the renderer fixture, with the debug layer and GPU-based validation.

#include "helpers/renderer_fixture.h"
#include "rendering/clusters.h"
#include "rendering/environment_baker.h"
#include "rendering/extraction.h"
#include "rendering/passes/lighting_passes.h"
#include "rendering/sky_model.h"
#include "rendering/view.h"

#include "avis/graphics/readback_data.h"
#include "avis/rhi/formats.h"

#include <Eigen/Core>
#include <Eigen/LU>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "shared_types.hlsli"

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::renderer_fixture;
using avis::tests::renderer_fixture_options;

namespace
{
    constexpr double pi = std::numbers::pi;

    // ---- reading captured texels ----

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

    // Channel `component` of a texel with `components` half floats (RGBA16F: 4, RG16F: 2)
    float half_at(
        const graphics::readback_image& image,
        const std::uint32_t x,
        const std::uint32_t y,
        const std::uint32_t components,
        const std::uint32_t component)
    {
        std::uint16_t value = 0;
        const std::size_t offset = static_cast<std::size_t>(y) * image.row_pitch() +
                                   (static_cast<std::size_t>(x) * components + component) * 2;
        std::memcpy(&value, image.bytes().data() + offset, sizeof(value));
        return half_to_float(value);
    }

    Eigen::Vector3f hdr_at(const graphics::readback_image& image, const std::uint32_t x, const std::uint32_t y)
    {
        return { half_at(image, x, y, 4, 0), half_at(image, x, y, 4, 1), half_at(image, x, y, 4, 2) };
    }

    float float_at(const graphics::readback_image& image, const std::uint32_t x, const std::uint32_t y)
    {
        float value = 0.0f;
        std::memcpy(
            &value,
            image.bytes().data() + static_cast<std::size_t>(y) * image.row_pitch() + static_cast<std::size_t>(x) * 4,
            sizeof(value));
        return value;
    }

    // ---- scenes and frames ----

    // Manual exposure at EV100 0: hdr_color = radiance / 1.2
    renderer_fixture_options lighting_options()
    {
        renderer_fixture_options options;
        options.settings.exposure.mode = exposure_mode::manual;
        options.settings.exposure.ev100 = 0.0f;
        return options;
    }

    view_description steady_view(renderer_fixture& fixture)
    {
        view_description made = fixture.view();
        made.options.taa = false;
        return made;
    }

    rhi::texture_handle capture(
        renderer_fixture& fixture, view_description& view, const std::string& resource, const rhi::format format)
    {
        const rhi::texture_handle texture = fixture.make_texture(format, view.size);
        view.options.captures.push_back({ .resource = resource, .texture = texture });
        return texture;
    }

    // The view-space direction through the centre of pixel (x, y) of the fixture's camera (60°, looking down −Z)
    Eigen::Vector3f pixel_direction(const view_description& view, const std::uint32_t x, const std::uint32_t y)
    {
        const float aspect = static_cast<float>(view.size.width) / static_cast<float>(view.size.height);
        const float scale = std::tan(0.5f * view.camera.vertical_field_of_view);
        const float ndc_x = (static_cast<float>(x) + 0.5f) / static_cast<float>(view.size.width) * 2.0f - 1.0f;
        const float ndc_y = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(view.size.height) * 2.0f;
        return Eigen::Vector3f{ ndc_x * scale * aspect, ndc_y * scale, -1.0f }.normalized();
    }

    scene::material_description white_lambertian()
    {
        scene::material_description material;
        material.base_color_factor = { 1.0f, 1.0f, 1.0f, 1.0f };
        material.metallic_factor = 0.0f;
        material.roughness_factor = 1.0f;
        return material;
    }

    // ---- CPU references ----

    double radical_inverse(std::uint32_t bits)
    {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return static_cast<double>(bits) * 2.3283064365386963e-10;
    }

    double visibility(const double n_dot_v, const double n_dot_l, const double alpha)
    {
        const double alpha2 = alpha * alpha;
        const double ggx_v = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0 - alpha2) + alpha2);
        const double ggx_l = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0 - alpha2) + alpha2);
        return 0.5 / (ggx_v + ggx_l);
    }

    // The split-sum LUT's (A, B) at (NoV, roughness), integrated with `samples` GGX samples (brdf.hlsli's formulas)
    std::array<double, 2> brdf_lut_reference(const double n_dot_v, const double roughness, const std::uint32_t samples)
    {
        const double clamped = std::clamp(roughness, 0.045, 1.0);
        const double alpha = clamped * clamped;
        const Eigen::Vector3d to_view{ std::sqrt(1.0 - n_dot_v * n_dot_v), 0.0, n_dot_v };
        std::array<double, 2> sum{ 0.0, 0.0 };
        for (std::uint32_t index = 0; index < samples; ++index)
        {
            const double u = (static_cast<double>(index) + 0.5) / samples;
            const double v = radical_inverse(index);
            const double phi = 2.0 * pi * u;
            const double cos_theta = std::sqrt((1.0 - v) / (1.0 + (alpha * alpha - 1.0) * v));
            const double sin_theta = std::sqrt(1.0 - cos_theta * cos_theta);
            const Eigen::Vector3d half{ sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta };
            const Eigen::Vector3d to_light = 2.0 * to_view.dot(half) * half - to_view;
            const double n_dot_l = std::clamp(to_light.z(), 0.0, 1.0);
            const double n_dot_h = std::clamp(half.z(), 0.0, 1.0);
            const double v_dot_h = std::clamp(to_view.dot(half), 0.0, 1.0);
            if (n_dot_l > 0.0 && n_dot_h > 0.0)
            {
                const double weighted = visibility(n_dot_v, n_dot_l, alpha) * 4.0 * n_dot_l * v_dot_h / n_dot_h;
                const double fresnel = std::pow(1.0 - v_dot_h, 5.0);
                sum[0] += (1.0 - fresnel) * weighted;
                sum[1] += fresnel * weighted;
            }
        }
        return { sum[0] / samples, sum[1] / samples };
    }

    // brdf.hlsli's evaluate_brdf for a white dielectric of `roughness`: the BRDF times NoL
    double white_brdf(
        const Eigen::Vector3d& normal,
        const Eigen::Vector3d& to_view,
        const Eigen::Vector3d& to_light,
        const double roughness)
    {
        const double n_dot_l = std::clamp(normal.dot(to_light), 0.0, 1.0);
        const double n_dot_v = std::max(normal.dot(to_view), 1e-4);
        const Eigen::Vector3d half = (to_view + to_light).normalized();
        const double n_dot_h = std::clamp(normal.dot(half), 0.0, 1.0);
        const double v_dot_h = std::clamp(to_view.dot(half), 0.0, 1.0);
        const double alpha = roughness * roughness;
        const double alpha2 = alpha * alpha;
        const double f = (n_dot_h * alpha2 - n_dot_h) * n_dot_h + 1.0;
        const double distribution = alpha2 / (pi * f * f);
        const double fresnel = 0.04 + 0.96 * std::pow(1.0 - v_dot_h, 5.0);
        const double specular = distribution * visibility(n_dot_v, n_dot_l, alpha) * fresnel;
        const double diffuse = (1.0 - fresnel) / pi;
        return (diffuse + specular) * n_dot_l;
    }

    // The exact sRGB encoding of a linear value, in RGBA8
    int srgb8(const double linear)
    {
        const double value = std::clamp(linear, 0.0, 1.0);
        const double encoded = value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
        return static_cast<int>(std::lround(encoded * 255.0));
    }
} // namespace

// ---- CPU ----

TEST_CASE("the sky model's zenith luminance matches Preetham's formula", "[renderer][unit]")
{
    // The sun at 45° elevation, turbidity 2.5
    environment_description environment;
    environment.settings.sun_direction = Eigen::Vector3f{ 1.0f, 0.0f, -1.0f }.normalized();
    environment.settings.sky_turbidity = 2.5f;
    const gpu::sky_constants sky = make_sky_constants(environment);
    CHECK(sky.mode == gpu::sky_mode_model);

    const double turbidity = 2.5;
    const double theta_sun = pi / 4.0;
    const double chi = (4.0 / 9.0 - turbidity / 120.0) * (pi - 2.0 * theta_sun);
    const double zenith = 1000.0 * ((4.0453 * turbidity - 4.9710) * std::tan(chi) - 0.2155 * turbidity + 2.4192);

    // Linear sRGB's luminance row recovers Y from the model's xyY
    const Eigen::Vector3f up = sky_radiance(sky, Eigen::Vector3f::UnitZ());
    const double luminance = 0.2126 * up.x() + 0.7152 * up.y() + 0.0722 * up.z();
    CHECK(luminance == Catch::Approx(zenith).epsilon(0.01));

    // Brighter around the sun than opposite it, and the ground below the horizon
    const Eigen::Vector3f towards_sun = sky_radiance(sky, Eigen::Vector3f{ -1.0f, 0.0f, 1.2f }.normalized());
    const Eigen::Vector3f away = sky_radiance(sky, Eigen::Vector3f{ 1.0f, 0.0f, 1.2f }.normalized());
    CHECK(towards_sun.sum() > away.sum());
    const Eigen::Vector3f ground = sky_radiance(sky, -Eigen::Vector3f::UnitZ());
    CHECK(ground == Eigen::Vector3f{ sky.ground_radiance.x, sky.ground_radiance.y, sky.ground_radiance.z });
    CHECK(ground.minCoeff() > 0.0f);

    // A uniform override is the same in every direction
    environment.uniform_radiance = Eigen::Vector3f{ 1.0f, 2.0f, 3.0f };
    const gpu::sky_constants uniform = make_sky_constants(environment);
    for (const Eigen::Vector3f& direction : { Eigen::Vector3f::UnitZ().eval(), (-Eigen::Vector3f::UnitZ()).eval() })
    {
        CHECK(sky_radiance(uniform, direction) == Eigen::Vector3f{ 1.0f, 2.0f, 3.0f });
    }
    CHECK_FALSE(environment == environment_description{});
}

TEST_CASE("the cluster grid's tiles and exponential slices", "[renderer][unit]")
{
    CHECK(cluster_tile_size({ 128, 72 }) == graphics::texture_extent{ 8, 8 });
    CHECK(cluster_tile_size({ 1920, 1080 }) == graphics::texture_extent{ 120, 120 });
    CHECK(cluster_tile_size({ 20, 10 }) == graphics::texture_extent{ 2, 2 });

    CHECK(cluster_slice(0.0f) == 0);
    CHECK(cluster_slice(gpu::cluster_near) == 0);
    CHECK(cluster_slice(1e6f) == gpu::cluster_count_z - 1);
    CHECK(cluster_slice_depth(0) == Catch::Approx(gpu::cluster_near));
    CHECK(cluster_slice_depth(gpu::cluster_count_z) == Catch::Approx(gpu::cluster_far));
    for (std::uint32_t slice = 0; slice < gpu::cluster_count_z; ++slice)
    {
        const float middle = std::sqrt(cluster_slice_depth(slice) * cluster_slice_depth(slice + 1));
        CHECK(cluster_slice(middle) == slice);
        CHECK(cluster_slice_depth(slice + 1) > cluster_slice_depth(slice));
    }
    CHECK(cluster_index(1, 2, 3) == 1 + 16 * (2 + 9 * 3));

    // A tile's bounds hold the points it covers, and a tile past the view's edge is empty
    const camera fixture_camera;
    const graphics::texture_extent size{ 128, 72 };
    const Eigen::Matrix4f inverse = fixture_camera.projection(128.0f / 72.0f).inverse();
    const cluster_box box = cluster_bounds(inverse, size, 8, 4, 10);
    const float depth = std::sqrt(cluster_slice_depth(10) * cluster_slice_depth(11));
    const Eigen::Vector3f centre_ray{ 0.0f, 0.0f, -1.0f }; // pixel (64, 36) is a corner of tile (8, 4)
    CHECK(sphere_overlaps_box(centre_ray * depth, 0.0f, box));
    CHECK_FALSE(sphere_overlaps_box(centre_ray * (cluster_slice_depth(12) * 1.01f), 0.0f, box));
    CHECK(cluster_bounds(inverse, { 20, 10 }, 15, 8, 0).empty());

    // The cone test: a cone pointing at a sphere touches it, pointing away doesn't
    const float cos_angle = std::cos(0.3f);
    const float sin_angle = std::sin(0.3f);
    CHECK(cone_overlaps_sphere(
        Eigen::Vector3f::Zero(), -Eigen::Vector3f::UnitZ(), 10.0f, cos_angle, sin_angle, { 0.0f, 0.0f, -5.0f }, 0.5f));
    CHECK_FALSE(cone_overlaps_sphere(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::UnitZ(), 10.0f, cos_angle, sin_angle, { 0.0f, 0.0f, -5.0f }, 0.5f));
}

TEST_CASE("a light without a range stops where its illuminance is negligible", "[renderer][unit]")
{
    const float range = light_cutoff_range(400.0f);
    CHECK(range == Catch::Approx(200.0f));
    CHECK(400.0f / (range * range) == Catch::Approx(light_cutoff_illuminance));
    CHECK(manual_exposure(0.0f) == Catch::Approx(1.0f / 1.2f));
    CHECK(manual_exposure(12.0f) == Catch::Approx(1.0f / (1.2f * 4096.0f)));
}

// ---- GPU ----

TEST_CASE("white furnace: a white sphere under a uniform environment reflects the environment", "[renderer]")
{
    renderer_fixture fixture{ lighting_options() };
    fixture.add_sphere({ 0.0f, 0.0f, -3.0f }, 1.0f, white_lambertian());
    fixture.publish();
    constexpr float radiance = 2.0f;
    fixture.renderer->set_environment_override(Eigen::Vector3f::Constant(radiance));
    // Without GTAO: with two slices it darkens the silhouette, seen at grazing angles, by up to 15%. The test checks
    // the lighting's energy balance; test_renderer_gtao.cpp checks GTAO.
    ambient_occlusion_settings no_ao = fixture.renderer->ao_parameters();
    no_ao.intensity = 0.0f;
    fixture.renderer->set_ao_parameters(no_ao);

    view_description main = steady_view(fixture);
    const rhi::texture_handle hdr = capture(fixture, main, "hdr_color", rhi::format::rgba16_float);
    const std::vector views{ main };
    fixture.render_and_read(views);

    const float expected = radiance * manual_exposure(0.0f);
    const graphics::readback_image image = fixture.read_subresource(hdr);
    float worst = 0.0f;
    for (std::uint32_t y = 0; y < image.height(); ++y)
    {
        for (std::uint32_t x = 0; x < image.width(); ++x)
        {
            const Eigen::Vector3f value = hdr_at(image, x, y);
            worst = std::max(worst, (value - Eigen::Vector3f::Constant(expected)).cwiseAbs().maxCoeff() / expected);
        }
    }
    INFO("worst relative error " << worst);
    CHECK(worst <= 0.05f);
}

TEST_CASE("a sun at 45 degrees on a white Lambertian plane", "[renderer]")
{
    renderer_fixture_options options;
    options.settings.exposure.ev100 = 12.0f;
    renderer_fixture fixture{ options };
    constexpr float illuminance = 100'000.0f;
    // A wall facing the camera, lit from 45° above
    fixture.add_lit_quad({ 0.0f, 0.0f, -5.0f }, 100.0f, 100.0f, white_lambertian());
    fixture.add_sun(Eigen::Vector3f{ 0.0f, -1.0f, -1.0f }, illuminance);
    fixture.publish();
    fixture.renderer->set_environment_override(Eigen::Vector3f::Zero()); // no ambient light

    view_description main = steady_view(fixture);
    const rhi::texture_handle hdr = capture(fixture, main, "hdr_color", rhi::format::rgba16_float);
    const std::vector views{ main };
    fixture.render_and_read(views);

    const std::uint32_t x = main.size.width / 2;
    const std::uint32_t y = main.size.height / 2;
    const float exposure = manual_exposure(12.0f);
    const float measured = hdr_at(fixture.read_subresource(hdr), x, y).x();

    // The BRDF of the shader, and the Lambert term the task names: the specular lobe and the Fresnel weight of the
    // diffuse part differ from Lambert by about 3% here
    const Eigen::Vector3d normal = Eigen::Vector3d::UnitZ();
    const Eigen::Vector3d to_view = (-pixel_direction(main, x, y)).cast<double>();
    const Eigen::Vector3d to_light = Eigen::Vector3d{ 0.0, 1.0, 1.0 }.normalized();
    const double full = illuminance * white_brdf(normal, to_view, to_light, 1.0) * exposure;
    const double lambert = illuminance * std::cos(pi / 4.0) / pi * exposure;
    INFO("measured " << measured << ", BRDF " << full << ", Lambert " << lambert);
    CHECK(measured == Catch::Approx(full).epsilon(0.02));
    CHECK(measured == Catch::Approx(lambert).epsilon(0.04));
}

TEST_CASE("a point light's falloff reaches zero at its range", "[renderer]")
{
    renderer_fixture fixture{ lighting_options() };
    constexpr float wall = 5.0f;
    constexpr float range = 2.0f;
    fixture.add_lit_quad({ 0.0f, 0.0f, -wall }, 100.0f, 100.0f, white_lambertian());
    const Eigen::Vector3f light{ 0.0f, 0.0f, -wall + 1.0f };
    fixture.add_point_light(light, 100.0f, range);
    fixture.publish();
    fixture.renderer->set_environment_override(Eigen::Vector3f::Zero());

    view_description main = steady_view(fixture);
    const rhi::texture_handle hdr = capture(fixture, main, "hdr_color", rhi::format::rgba16_float);
    const std::vector views{ main };
    fixture.render_and_read(views);

    const graphics::readback_image image = fixture.read_subresource(hdr);
    std::size_t lit = 0;
    std::size_t dark = 0;
    for (std::uint32_t y = 0; y < image.height(); ++y)
    {
        for (std::uint32_t x = 0; x < image.width(); ++x)
        {
            const Eigen::Vector3f direction = pixel_direction(main, x, y);
            const Eigen::Vector3f on_wall = direction * (wall / -direction.z());
            const float distance = (on_wall - light).norm();
            const float value = hdr_at(image, x, y).x();
            INFO("pixel " << x << ", " << y << " at " << distance << " m");
            if (distance > range * 1.001f)
            {
                CHECK(value == 0.0f);
                ++dark;
            }
            else if (distance < range * 0.9f)
            {
                CHECK(value > 0.0f);
                ++lit;
            }
        }
    }
    CHECK(lit > 0);
    CHECK(dark > 0);
}

TEST_CASE("light clusters match a CPU brute force", "[renderer]")
{
    renderer_fixture fixture{ lighting_options() };
    fixture.add_ground_plane(200.0f);
    fixture.add_unit_cube({ 0.5f, 0.5f, -6.0f });

    // 64 point lights in and around the view
    std::mt19937 random{ 1234 };
    std::uniform_real_distribution<float> across{ -8.0f, 8.0f };
    std::uniform_real_distribution<float> height{ -1.0f, 5.0f };
    std::uniform_real_distribution<float> along{ -40.0f, -1.0f };
    std::uniform_real_distribution<float> ranges{ 0.5f, 4.0f };
    std::vector<std::pair<Eigen::Vector3f, float>> lights;
    for (int index = 0; index < 64; ++index)
    {
        const Eigen::Vector3f position{ across(random), height(random), along(random) };
        const float range = ranges(random);
        fixture.add_point_light(position, 100.0f, range);
        lights.push_back({ position, range });
    }
    fixture.publish();

    view_description main = steady_view(fixture);
    main.camera.world_from_view.translation().y() = 1.5f;
    const rhi::texture_handle linear_depth = capture(fixture, main, "linear_depth", rhi::format::r32_float);
    const rhi::buffer_handle cluster_buffer =
        fixture.make_readback_buffer(std::uint64_t{ gpu::cluster_count } * sizeof(gpu::cluster_lights));
    const rhi::buffer_handle index_buffer = fixture.make_readback_buffer(cluster_light_index_count * 4);
    main.options.buffer_captures.push_back({ .resource = "cluster_lights", .buffer = cluster_buffer });
    main.options.buffer_captures.push_back({ .resource = "light_indices", .buffer = index_buffer });
    const std::vector views{ main };
    fixture.render_and_read(views);

    std::vector<gpu::cluster_lights> measured(gpu::cluster_count);
    std::memcpy(
        measured.data(), fixture.mapped_bytes(cluster_buffer).data(), measured.size() * sizeof(gpu::cluster_lights));
    std::vector<std::uint32_t> indices(cluster_light_index_count);
    std::memcpy(indices.data(), fixture.mapped_bytes(index_buffer).data(), indices.size() * 4);
    const graphics::readback_image depth = fixture.read_subresource(linear_depth);

    // At 128 × 72 a tile is 8 × 8 pixels, exactly one texel of depth_mips' mip 3: the tile's depth range is exact
    const graphics::texture_extent tile = cluster_tile_size(main.size);
    REQUIRE(tile == graphics::texture_extent{ 8, 8 });
    const Eigen::Matrix4f view_from_world = main.camera.view_from_world();
    const Eigen::Matrix4f inverse = main.camera.projection(128.0f / 72.0f).inverse();
    constexpr float infinity = std::numeric_limits<float>::infinity();

    std::size_t compared = 0;
    std::size_t skipped = 0;
    std::size_t with_lights = 0;
    for (std::uint32_t z = 0; z < gpu::cluster_count_z; ++z)
    {
        for (std::uint32_t y = 0; y < gpu::cluster_count_y; ++y)
        {
            for (std::uint32_t x = 0; x < gpu::cluster_count_x; ++x)
            {
                const std::uint32_t index = cluster_index(x, y, z);
                const gpu::cluster_lights& cluster = measured[index];
                INFO("cluster " << x << ", " << y << ", " << z);
                REQUIRE(cluster.offset == index * gpu::cluster_maximum_lights);
                CHECK(cluster.dropped == 0);

                float tile_minimum = infinity;
                float tile_maximum = -infinity;
                for (std::uint32_t row = y * tile.height; row < (y + 1) * tile.height; ++row)
                {
                    for (std::uint32_t column = x * tile.width; column < (x + 1) * tile.width; ++column)
                    {
                        const float value = float_at(depth, column, row);
                        tile_minimum = std::min(tile_minimum, value);
                        tile_maximum = std::max(tile_maximum, value);
                    }
                }
                // The slice's depths, widened and narrowed by the CPU and GPU's rounding: a tile near a slice boundary
                // isn't compared
                const float slice_near = z == 0 ? 0.0f : cluster_slice_depth(z);
                const float slice_far = z == gpu::cluster_count_z - 1 ? infinity : cluster_slice_depth(z + 1);
                const bool skip_certain = std::isinf(tile_minimum) || tile_minimum > slice_far * 1.0001f ||
                                          tile_maximum < slice_near * 0.9999f;
                const bool keep_certain = !std::isinf(tile_minimum) && tile_minimum < slice_far * 0.9999f &&
                                          tile_maximum > slice_near * 1.0001f;
                if (skip_certain)
                {
                    CHECK(cluster.count == 0);
                    ++skipped;
                    continue;
                }
                if (!keep_certain)
                {
                    continue;
                }

                const cluster_box box = cluster_bounds(inverse, main.size, x, y, z);
                std::vector<std::uint32_t> inner;
                std::vector<std::uint32_t> outer;
                for (std::uint32_t light = 0; light < lights.size(); ++light)
                {
                    const Eigen::Vector3f centre = (view_from_world * lights[light].first.homogeneous()).head<3>();
                    const float radius = lights[light].second;
                    if (sphere_overlaps_box(centre, radius * 0.999f - 1e-3f, box))
                    {
                        inner.push_back(light);
                    }
                    if (sphere_overlaps_box(centre, radius * 1.001f + 1e-3f, box))
                    {
                        outer.push_back(light);
                    }
                }
                CHECK(cluster.count >= inner.size());
                CHECK(cluster.count <= outer.size());
                if (inner == outer)
                {
                    const std::vector<std::uint32_t> listed(
                        indices.begin() + cluster.offset, indices.begin() + cluster.offset + cluster.count);
                    CHECK(listed == inner); // in light order
                }
                with_lights += cluster.count > 0 ? 1 : 0;
                ++compared;
            }
        }
    }
    INFO("compared " << compared << " clusters, " << skipped << " skipped by depth, " << with_lights << " lit");
    CHECK(compared > 0);
    CHECK(skipped > 0);
    CHECK(with_lights > 0);
}

TEST_CASE_METHOD(renderer_fixture, "the BRDF LUT matches a CPU integration", "[renderer]")
{
    publish();
    const std::vector views{ view() };
    render_and_read(views);

    const graphics::readback_image lut = read_subresource(renderer->environment().brdf_lut_texture());
    REQUIRE(lut.width() == gpu::brdf_lut_size);
    // Texel (i, j) is NoV = (i + 0.5) / 128, roughness = (j + 0.5) / 128: (64, 64) is about (0.5, 0.5), (127, 0) about
    // (1, 0), where the split sum is about (1, 0)
    for (const auto& [i, j] : { std::pair{ 64u, 64u }, std::pair{ 127u, 0u }, std::pair{ 20u, 100u } })
    {
        const double n_dot_v = (i + 0.5) / gpu::brdf_lut_size;
        const double roughness = (j + 0.5) / gpu::brdf_lut_size;
        const std::array<double, 2> expected = brdf_lut_reference(n_dot_v, roughness, 1u << 16);
        INFO("texel " << i << ", " << j << ": expected " << expected[0] << ", " << expected[1]);
        CHECK(half_at(lut, i, j, 2, 0) == Catch::Approx(expected[0]).margin(1e-2));
        CHECK(half_at(lut, i, j, 2, 1) == Catch::Approx(expected[1]).margin(1e-2));
    }
    CHECK(half_at(lut, 127, 0, 2, 0) == Catch::Approx(1.0).margin(2e-2));
    CHECK(half_at(lut, 127, 0, 2, 1) == Catch::Approx(0.0).margin(1e-2));
}

TEST_CASE_METHOD(renderer_fixture, "the environment is baked once, and again when it changes", "[renderer]")
{
    add_sun({ 1.0f, 0.0f, -1.0f });
    publish();
    const std::vector views{ view() };
    render_and_read(views, 10);
    CHECK(renderer->environment().statistics().bakes == 1);
    CHECK(renderer->environment().statistics().brdf_lut_bakes == 1);

    scene::environment_settings environment = world.environment();
    environment.sun_direction = Eigen::Vector3f{ -1.0f, 0.5f, -0.5f }.normalized();
    world.set_environment(environment);
    publish();
    render_and_read(views, 3);
    CHECK(renderer->environment().statistics().bakes == 2);
    CHECK(renderer->environment().statistics().brdf_lut_bakes == 1);
    CHECK(renderer->debug_information().environment.bakes == 2);

    // A view that writes nothing doesn't bake
    renderer->set_environment_override(Eigen::Vector3f::Ones());
    view_description hidden = view("hidden");
    hidden.output.reset();
    const std::vector hidden_views{ hidden };
    REQUIRE(render_frame(hidden_views).has_value());
    CHECK(renderer->environment().statistics().bakes == 2);
    render_and_read(views);
    CHECK(renderer->environment().statistics().bakes == 3);
}
