// GTAO on the GPU (docs/tasks/11-renderer.md §11.13): a plane facing the camera, a 90° concave corner against a CPU
// reference of the same estimator, half against full resolution, and the 11 §15 time target. On the renderer fixture,
// with the debug layer and GPU-based validation (except the benchmark).
//
// The corner is a V-shaped trough facing the camera: two planes at 45° to the view axis meeting in a vertical crease
// 4 m in front of the camera. Its depth at every pixel is analytic, so the reference runs the estimator of gtao.hlsl on
// the CPU with exact depths, averaged over a grid of noise values instead of the shader's per-texel noise. The
// conventions call for a WARP-rendered reference image here; a reference computed from the scene's geometry works on
// any adapter, and tests the same thing.

#include "helpers/renderer_fixture.h"
#include "rendering/passes/gtao_pass.h"
#include "rendering/view.h"

#include "avis/graphics/readback_data.h"
#include "avis/rhi/formats.h"
#include "avis/scene/assets/mesh_builder.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string_view>
#include <vector>

#include "shared_types.hlsli"

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::renderer_fixture;
using avis::tests::renderer_fixture_options;

namespace
{
    constexpr float crease_depth = 4.0f; // metres in front of the camera
    constexpr float trough_half_width = 2.5f; // metres along x: past the view's edges
    constexpr float trough_half_height = 3.0f; // metres along y: past the view's edges
    constexpr float infinity = std::numeric_limits<float>::infinity();

    view_description steady_view(renderer_fixture& fixture)
    {
        view_description made = fixture.view();
        made.options.taa = false; // no jitter: the reference uses the unjittered camera
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

    // The trough: x + z = −4 for x ≤ 0 and z − x = −4 for x ≥ 0, facing the camera
    scene::mesh_data make_trough()
    {
        scene::mesh_builder builder{ scene::mesh_topology::triangles };
        const float h = trough_half_height;
        const float w = trough_half_width;
        const Eigen::Vector3f left_normal = Eigen::Vector3f{ 1.0f, 0.0f, 1.0f }.normalized();
        const Eigen::Vector3f right_normal = Eigen::Vector3f{ -1.0f, 0.0f, 1.0f }.normalized();
        // Counter-clockwise seen from the camera
        const std::array<Eigen::Vector3f, 4> left{ Eigen::Vector3f{ -w, -h, -crease_depth + w },
                                                   Eigen::Vector3f{ 0.0f, -h, -crease_depth },
                                                   Eigen::Vector3f{ 0.0f, h, -crease_depth },
                                                   Eigen::Vector3f{ -w, h, -crease_depth + w } };
        const std::array<Eigen::Vector3f, 4> right{ Eigen::Vector3f{ 0.0f, -h, -crease_depth },
                                                    Eigen::Vector3f{ w, -h, -crease_depth + w },
                                                    Eigen::Vector3f{ w, h, -crease_depth + w },
                                                    Eigen::Vector3f{ 0.0f, h, -crease_depth } };
        std::uint32_t first = 0;
        for (const auto& [corners, normal] : { std::pair{ left, left_normal }, std::pair{ right, right_normal } })
        {
            for (const Eigen::Vector3f& corner : corners)
            {
                builder.add_vertex(corner).normal(normal);
            }
            for (const std::uint32_t index : { 0u, 1u, 2u, 0u, 2u, 3u })
            {
                builder.add_index(first + index);
            }
            first += 4;
        }
        result<scene::mesh_data> built = std::move(builder).build();
        REQUIRE(built.has_value());
        return std::move(*built);
    }

    // The fixture's camera (at the origin looking down −Z, 60° vertical field of view, no jitter)
    struct pinhole
    {
        float width = 0.0f;
        float height = 0.0f;
        float tan_half = 0.0f;

        explicit pinhole(const view_description& view) :
            width{ static_cast<float>(view.size.width) },
            height{ static_cast<float>(view.size.height) },
            tan_half{ std::tan(0.5f * view.camera.vertical_field_of_view) }
        {}

        // The view-space point at linear depth 1 through the continuous pixel position `pixel`
        Eigen::Vector3f ray(const Eigen::Vector2f& pixel) const
        {
            const float ndc_x = pixel.x() / width * 2.0f - 1.0f;
            const float ndc_y = 1.0f - pixel.y() / height * 2.0f;
            return { ndc_x * tan_half * width / height, ndc_y * tan_half, -1.0f };
        }
    };

    // The trough's linear depth through `pixel`, infinity where it isn't drawn
    float trough_depth(const pinhole& view, const Eigen::Vector2f& pixel)
    {
        const Eigen::Vector3f ray = view.ray(pixel);
        const float depth = crease_depth / (1.0f + std::abs(ray.x()));
        if (std::abs(depth * ray.x()) > trough_half_width || std::abs(depth * ray.y()) > trough_half_height)
        {
            return infinity;
        }
        return depth;
    }

    // The distance from the trough's point through `pixel` to the crease, along its plane
    float crease_distance(const pinhole& view, const Eigen::Vector2f& pixel)
    {
        return std::abs(trough_depth(view, pixel) * view.ray(pixel).x()) * std::numbers::sqrt2_v<float>;
    }

    // gtao.hlsl's estimator at the pixel with centre `centre`, for the trough, with the noise values `noise`
    float reference_visibility(
        const pinhole& view,
        const ambient_occlusion_settings& parameters,
        const Eigen::Vector2f& centre,
        const Eigen::Vector2f& noise)
    {
        constexpr float pi = std::numbers::pi_v<float>;
        constexpr float half_pi = 0.5f * pi;
        const float depth = trough_depth(view, centre);
        const Eigen::Vector3f position = view.ray(centre) * depth;
        const Eigen::Vector3f view_vector = -position.normalized();
        const Eigen::Vector3f normal = position.x() <= 0.0f ? Eigen::Vector3f{ 1.0f, 0.0f, 1.0f }.normalized() :
                                                              Eigen::Vector3f{ -1.0f, 0.0f, 1.0f }.normalized();
        const float radius_pixels = parameters.radius * 0.5f * view.height / view.tan_half / depth;
        const float falloff_range = parameters.falloff * parameters.radius;
        const float falloff_scale = -1.0f / falloff_range;
        const float falloff_offset = (parameters.radius - falloff_range) / falloff_range + 1.0f;
        const float minimum_step = 1.3f / radius_pixels;
        const Eigen::Vector3f basis_x = Eigen::Vector3f{ view_vector.z(), 0.0f, -view_vector.x() }.normalized();
        const Eigen::Vector3f basis_y = view_vector.cross(basis_x);

        // The shader interpolates the depth mips at the sample; the reference has the exact depth there
        const auto horizon = [&](const Eigen::Vector2f& pixel, const float low)
        {
            const Eigen::Vector2f sample_centre{ std::clamp(pixel.x(), 0.5f, view.width - 0.5f),
                                                 std::clamp(pixel.y(), 0.5f, view.height - 0.5f) };
            const float sample_depth = trough_depth(view, sample_centre);
            if (!(sample_depth < infinity))
            {
                return low;
            }
            const Eigen::Vector3f delta = view.ray(sample_centre) * sample_depth - position;
            const float distance = delta.norm();
            if (!(distance > 0.0f))
            {
                return low;
            }
            const float weight = std::clamp(distance * falloff_scale + falloff_offset, 0.0f, 1.0f);
            return low + (delta.dot(view_vector) / distance - low) * weight;
        };

        float visibility = 0.0f;
        for (std::uint32_t slice = 0; slice < parameters.slices; ++slice)
        {
            const float psi = (static_cast<float>(slice) + noise.x()) * pi / static_cast<float>(parameters.slices);
            const Eigen::Vector3f ortho_direction = std::cos(psi) * basis_x + std::sin(psi) * basis_y;
            const Eigen::Vector2f omega =
                Eigen::Vector2f{ ortho_direction.x() * -position.z() + position.x() * ortho_direction.z(),
                                 -(ortho_direction.y() * -position.z() + position.y() * ortho_direction.z()) }
                    .normalized();
            const Eigen::Vector3f axis = ortho_direction.cross(view_vector).normalized();
            const Eigen::Vector3f projected_normal = normal - axis * normal.dot(axis);
            const float projected_length = projected_normal.norm();
            const float sign_n = ortho_direction.dot(projected_normal) >= 0.0f ? 1.0f : -1.0f;
            const float cos_n = std::clamp(projected_normal.dot(view_vector) / projected_length, 0.0f, 1.0f);
            const float n = sign_n * std::acos(cos_n);
            const float sin_n = std::sin(n);
            const float low_positive = std::cos(n + half_pi);
            const float low_negative = std::cos(n - half_pi);
            float horizon_positive = low_positive;
            float horizon_negative = low_negative;
            for (std::uint32_t step = 0; step < parameters.steps; ++step)
            {
                float s = (static_cast<float>(step) + noise.y()) / static_cast<float>(parameters.steps);
                s = s * s + minimum_step;
                const Eigen::Vector2f offset = s * radius_pixels * omega;
                horizon_positive = std::max(horizon_positive, horizon(centre + offset, low_positive));
                horizon_negative = std::max(horizon_negative, horizon(centre - offset, low_negative));
            }
            float h_negative = -std::acos(std::clamp(horizon_negative, -1.0f, 1.0f));
            float h_positive = std::acos(std::clamp(horizon_positive, -1.0f, 1.0f));
            h_negative = n + std::clamp(h_negative - n, -half_pi, half_pi);
            h_positive = n + std::clamp(h_positive - n, -half_pi, half_pi);
            const auto arc = [&](const float h) { return 0.25f * (cos_n + 2.0f * h * sin_n - std::cos(2.0f * h - n)); };
            visibility += projected_length * (arc(h_negative) + arc(h_positive));
        }
        return std::pow(
            std::clamp(visibility / static_cast<float>(parameters.slices), 0.0f, 1.0f), parameters.intensity);
    }

    // The expected value of the estimator at `centre` over the shader's noise: a grid of noise values
    float reference_ao(const pinhole& view, const ambient_occlusion_settings& parameters, const Eigen::Vector2f& centre)
    {
        constexpr int slice_noise = 16;
        constexpr int step_noise = 8;
        float sum = 0.0f;
        for (int i = 0; i < slice_noise; ++i)
        {
            for (int j = 0; j < step_noise; ++j)
            {
                const Eigen::Vector2f noise{ (static_cast<float>(i) + 0.5f) / slice_noise,
                                             (static_cast<float>(j) + 0.5f) / step_noise };
                sum += reference_visibility(view, parameters, centre, noise);
            }
        }
        return sum / (slice_noise * step_noise);
    }

    // The denoised AO of one frame, one value per AO texel
    struct ao_image
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t scale = 1; // pixels per texel
        std::vector<float> values;

        float at(const std::uint32_t x, const std::uint32_t y) const
        {
            return values[static_cast<std::size_t>(y) * width + x];
        }

        // The centre of the pixel texel (x, y) is computed at
        Eigen::Vector2f centre(const std::uint32_t x, const std::uint32_t y) const
        {
            return { static_cast<float>(x * scale) + 0.5f, static_cast<float>(y * scale) + 0.5f };
        }
    };

    // Renders one frame of `view` with the renderer's AO parameters and reads `resource` (ao, ao_raw)
    ao_image render_ao(renderer_fixture& fixture, view_description view, const std::string_view resource = "ao")
    {
        const ambient_occlusion_settings& parameters = fixture.renderer->ao_parameters();
        const graphics::texture_extent size = gtao_extent(parameters, view.size);
        const rhi::texture_handle texture = fixture.make_texture(rhi::format::r8_unorm, size);
        view.options.captures.clear();
        view.options.captures.push_back({ .resource = std::string{ resource }, .texture = texture });
        const std::vector views{ view };
        fixture.render_and_read(views);

        ao_image image{ .width = size.width, .height = size.height, .scale = gtao_scale(parameters) };
        const graphics::readback_image read = fixture.read_subresource(texture);
        image.values.resize(static_cast<std::size_t>(size.width) * size.height);
        for (std::uint32_t y = 0; y < size.height; ++y)
        {
            for (std::uint32_t x = 0; x < size.width; ++x)
            {
                const auto byte = static_cast<std::uint8_t>(read.bytes()[y * read.row_pitch() + x]);
                image.values[static_cast<std::size_t>(y) * size.width + x] = static_cast<float>(byte) / 255.0f;
            }
        }
        return image;
    }

    void set_half_resolution(renderer_fixture& fixture, const bool half)
    {
        ambient_occlusion_settings parameters = fixture.renderer->ao_parameters();
        parameters.half_resolution = half;
        fixture.renderer->set_ao_parameters(parameters);
    }

    // AO by distance to the crease, in bins of bin_width metres
    constexpr float bin_width = 0.15f;
    constexpr std::size_t bin_count = 16; // up to 2.4 m

    struct binned
    {
        std::array<double, bin_count> sum{};
        std::array<std::size_t, bin_count> count{};

        void add(const float distance, const float value)
        {
            const auto bin = static_cast<std::size_t>(distance / bin_width);
            if (bin < bin_count)
            {
                sum[bin] += value;
                ++count[bin];
            }
        }

        double mean(const std::size_t bin) const
        {
            return count[bin] == 0 ? 0.0 : sum[bin] / static_cast<double>(count[bin]);
        }
    };

    // The mean AO over the trough's texels, `margin` texels away from the image's edges
    double mean_ao(const ao_image& image, const std::uint32_t margin)
    {
        double sum = 0.0;
        std::size_t count = 0;
        for (std::uint32_t y = margin; y + margin < image.height; ++y)
        {
            for (std::uint32_t x = margin; x + margin < image.width; ++x)
            {
                sum += image.at(x, y);
                ++count;
            }
        }
        return sum / static_cast<double>(count);
    }
} // namespace

TEST_CASE("GTAO of a flat plane facing the camera is 1 away from the edges", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_lit_quad({ 0.0f, 0.0f, -4.0f }, 100.0f, 100.0f, white_lambertian());
    fixture.publish();
    const view_description main = steady_view(fixture);

    for (const bool half : { true, false })
    {
        CAPTURE(half);
        set_half_resolution(fixture, half);
        const ao_image image = render_ao(fixture, main);
        // 8 pixels from the edges, where the view's rays are within 45° of the plane's normal: further out, two slices
        // see the plane at angles where their visibility sum falls below 0.98
        const std::uint32_t margin = 8 / image.scale;
        float lowest = 1.0f;
        for (std::uint32_t y = margin; y + margin < image.height; ++y)
        {
            for (std::uint32_t x = margin; x + margin < image.width; ++x)
            {
                lowest = std::min(lowest, image.at(x, y));
            }
        }
        CHECK(lowest >= 0.98f);
    }
}

TEST_CASE("GTAO of a 90 degree concave corner falls towards the crease and matches the reference", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_mesh("trough", Eigen::Vector3f::Zero(), make_trough(), white_lambertian());
    fixture.publish();
    const view_description main = steady_view(fixture);
    const pinhole view{ main };

    for (const bool half : { false, true })
    {
        CAPTURE(half);
        set_half_resolution(fixture, half);
        const ambient_occlusion_settings parameters = fixture.renderer->ao_parameters();
        const ao_image image = render_ao(fixture, main);

        // The rows near the top and bottom are left out: the denoiser has fewer neighbours there
        constexpr std::uint32_t margin_pixels = 4;
        binned measured;
        binned reference;
        for (std::uint32_t y = 0; y < image.height; ++y)
        {
            for (std::uint32_t x = 0; x < image.width; ++x)
            {
                const Eigen::Vector2f centre = image.centre(x, y);
                if (centre.y() < margin_pixels || centre.y() > view.height - margin_pixels ||
                    centre.x() < margin_pixels || centre.x() > view.width - margin_pixels)
                {
                    continue;
                }
                REQUIRE(trough_depth(view, centre) < infinity);
                const float distance = crease_distance(view, centre);
                measured.add(distance, image.at(x, y));
                // The reference on every other pixel of every other row: its bins are wide enough
                if ((x * image.scale) % 2 == 0 && (y * image.scale) % 2 == 0)
                {
                    reference.add(distance, reference_ao(view, parameters, centre));
                }
            }
        }

        for (std::size_t bin = 0; bin < bin_count; ++bin)
        {
            if (measured.count[bin] == 0 || reference.count[bin] == 0)
            {
                continue;
            }
            UNSCOPED_INFO(
                "bin " << bin << " (" << bin * bin_width << " m): measured " << measured.mean(bin) << " over "
                       << measured.count[bin] << ", reference " << reference.mean(bin));
        }
        // Falls towards the crease: about half the hemisphere is the other plane there, nothing beyond the radius
        CHECK(measured.mean(0) < 0.75);
        CHECK(measured.mean(bin_count - 1) > 0.95);
        for (std::size_t bin = 0; bin + 1 < bin_count; ++bin)
        {
            CAPTURE(bin);
            CHECK(measured.mean(bin + 1) >= measured.mean(bin) - 0.02);
        }
        // Matches the reference within 0.05 per bin: the denoiser spreads each value over about ±2 pixels, which the
        // bins' width covers except at the crease, where the reference is steepest
        for (std::size_t bin = 0; bin < bin_count; ++bin)
        {
            CAPTURE(bin);
            REQUIRE(measured.count[bin] > 0);
            REQUIRE(reference.count[bin] > 0);
            CHECK(std::abs(measured.mean(bin) - reference.mean(bin)) <= (bin == 0 ? 0.08 : 0.05));
        }
    }
}

TEST_CASE("GTAO at half and full resolution give the same mean on the corner", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_mesh("trough", Eigen::Vector3f::Zero(), make_trough(), white_lambertian());
    fixture.publish();
    const view_description main = steady_view(fixture);

    set_half_resolution(fixture, true);
    const double half = mean_ao(render_ao(fixture, main), 2);
    set_half_resolution(fixture, false);
    const double full = mean_ao(render_ao(fixture, main), 4);
    UNSCOPED_INFO("half " << half << ", full " << full);
    CHECK(std::abs(half - full) <= 0.03 * full);
    CHECK(full < 0.97); // the corner is occluded (most of the trough is beyond the radius)
}

TEST_CASE("GTAO is not added at intensity 0", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_lit_quad({ 0.0f, 0.0f, -4.0f }, 100.0f, 100.0f, white_lambertian());
    fixture.publish();
    ambient_occlusion_settings parameters = fixture.renderer->ao_parameters();
    parameters.intensity = 0.0f;
    fixture.renderer->set_ao_parameters(parameters);

    view_description main = steady_view(fixture);
    main.options.captures.push_back(
        { .resource = "ao",
          .texture = fixture.make_texture(rhi::format::r8_unorm, gtao_extent(parameters, main.size)) });
    const std::vector views{ main };
    const result<graphics::submitted_frame> rendered = fixture.render_frame(views);
    REQUIRE_FALSE(rendered.has_value());
    CHECK(rendered.error().message.find("'ao' isn't rendered") != std::string::npos);
}

TEST_CASE("GTAO at half resolution takes under 0.8 ms at 1920 x 1080", "[renderer][!benchmark][.]")
{
    renderer_fixture_options options;
    options.graph.gpu_based_validation = false;
    options.graph.profiler = true;
    options.output_size = { 1920, 1080 };
    renderer_fixture fixture{ options };
    fixture.add_mesh("trough", Eigen::Vector3f::Zero(), make_trough(), white_lambertian());
    fixture.publish();
    REQUIRE(fixture.renderer->ao_parameters().half_resolution);

    const std::vector views{ steady_view(fixture) };
    double best = std::numeric_limits<double>::infinity();
    for (int frame = 0; frame < 8; ++frame)
    {
        fixture.render_and_read(views);
        double total = 0.0;
        bool timed = false;
        for (const graphics::pass_debug_information& pass : fixture.graphics.graph.debug_information().passes)
        {
            if ((pass.name.ends_with("gtao") || pass.name.find("gtao_denoise_") != std::string_view::npos) &&
                pass.gpu_milliseconds)
            {
                total += *pass.gpu_milliseconds;
                timed = true;
            }
        }
        if (timed)
        {
            best = std::min(best, total);
        }
    }
    REQUIRE(best < std::numeric_limits<double>::infinity());
    UNSCOPED_INFO("GTAO + denoise: " << best << " ms");
    CHECK(best < 0.8);
}
