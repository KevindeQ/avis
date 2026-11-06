// The TAA resolve (docs/tasks/11-renderer.md §11.15): a static scene converges and its edges are anti-aliased, a
// moving cube leaves no ghost trail, a history reset gives the unaccumulated jittered frame, and with TAA off
// taa_output is hdr_color. On the renderer fixture, with the debug layer and GPU-based validation.

#include "helpers/renderer_fixture.h"
#include "rendering/view.h"

#include "avis/graphics/readback_data.h"
#include "avis/rhi/formats.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

    // The RGB of every texel of an RGBA16F image, row-major; copied out, since the image's bytes belong to the fixture
    struct hdr_image
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<Eigen::Vector3f> pixels;

        const Eigen::Vector3f& at(const std::uint32_t x, const std::uint32_t y) const
        {
            return pixels[static_cast<std::size_t>(y) * width + x];
        }
    };

    hdr_image read_hdr(renderer_fixture& fixture, const rhi::texture_handle texture)
    {
        const graphics::readback_image image = fixture.read_subresource(texture);
        hdr_image read{ .width = image.width(), .height = image.height(), .pixels = {} };
        for (std::uint32_t y = 0; y < image.height(); ++y)
        {
            for (std::uint32_t x = 0; x < image.width(); ++x)
            {
                Eigen::Vector3f value;
                for (int component = 0; component < 3; ++component)
                {
                    std::uint16_t half = 0;
                    std::memcpy(
                        &half,
                        image.bytes().data() + static_cast<std::size_t>(y) * image.row_pitch() +
                            (static_cast<std::size_t>(x) * 4 + static_cast<std::size_t>(component)) * 2,
                        sizeof(half));
                    value[component] = half_to_float(half);
                }
                read.pixels.push_back(value);
            }
        }
        return read;
    }

    // Manual exposure with a pre-exposure of 1: 1 / (1.2 · 2^EV100) = 1
    renderer_fixture_options unit_exposure()
    {
        renderer_fixture_options options;
        options.settings.exposure.mode = exposure_mode::manual;
        options.settings.exposure.ev100 = -std::log2(1.2f);
        return options;
    }

    // A view with TAA under a black sky, without GTAO
    view_description taa_view(renderer_fixture& fixture)
    {
        fixture.renderer->set_environment_override(Eigen::Vector3f::Zero());
        ambient_occlusion_settings no_ao = fixture.renderer->ao_parameters();
        no_ao.intensity = 0.0f;
        fixture.renderer->set_ao_parameters(no_ao);
        view_description made = fixture.view();
        made.options.taa = true;
        return made;
    }

    rhi::texture_handle capture(
        renderer_fixture& fixture, view_description& view, const std::string& resource, const rhi::format format)
    {
        const rhi::texture_handle texture = fixture.make_texture(format, view.size);
        view.options.captures.push_back({ .resource = resource, .texture = texture });
        return texture;
    }

    scene::material_description unlit(const scene::color color)
    {
        scene::material_description material;
        material.base_color_factor = color;
        material.shading = scene::shading_model::unlit;
        return material;
    }

    // The metres one pixel spans at `distance` in front of the fixture's camera (60° vertical field of view)
    float pixel_metres(const float distance, const std::uint32_t height)
    {
        return 2.0f * distance * std::tan(0.5f * 1.0471976f) / static_cast<float>(height);
    }

    // A white unlit line 1.5 px wide and 4 m long at 5 m, turned 30° from the horizontal, over the black sky
    void add_diagonal_line(renderer_fixture& fixture)
    {
        constexpr float distance = 5.0f;
        const float width = 1.5f * pixel_metres(distance, fixture.output_size.height);
        const scene::entity line =
            fixture.add_lit_quad({ 0.0f, 0.0f, -distance }, 4.0f, width, unlit({ 1.0f, 1.0f, 1.0f, 1.0f }));
        scene::transform local = fixture.world.local(line);
        local.rotation = Eigen::Quaternionf{ Eigen::AngleAxisf{ 0.5235988f, Eigen::Vector3f::UnitZ() } };
        fixture.world.set_local(line, local);
    }

    // The pixels whose red is strictly between the background (0) and the line (1), with a margin
    std::size_t count_intermediate(const hdr_image& image)
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            image.pixels, [](const Eigen::Vector3f& pixel) { return pixel.x() > 0.05f && pixel.x() < 0.95f; }));
    }

    std::size_t count_lit(const hdr_image& image)
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(image.pixels, [](const Eigen::Vector3f& pixel) { return pixel.x() >= 0.95f; }));
    }
} // namespace

TEST_CASE("TAA converges on a static scene and anti-aliases a thin diagonal line", "[renderer]")
{
    renderer_fixture fixture{ unit_exposure() };
    add_diagonal_line(fixture);
    fixture.publish();

    // Without TAA, every pixel is the line or the background: hard steps
    {
        view_description plain = taa_view(fixture);
        plain.options.taa = false;
        const rhi::texture_handle hdr = capture(fixture, plain, "hdr_color", rhi::format::rgba16_float);
        const std::vector views{ plain };
        fixture.render_and_read(views);
        const hdr_image image = read_hdr(fixture, hdr);
        INFO("line pixels " << count_lit(image));
        CHECK(count_lit(image) > 50);
        CHECK(count_intermediate(image) == 0);
    }

    view_description main = taa_view(fixture);
    const rhi::texture_handle resolved = capture(fixture, main, "taa_output", rhi::format::rgba16_float);
    const std::vector views{ main };
    fixture.render_and_read(views, 31);
    const std::vector<tests::rgba8_image> before = fixture.render_and_read(views, 1); // frame 32
    const hdr_image converged = read_hdr(fixture, resolved);
    const std::vector<tests::rgba8_image> after = fixture.render_and_read(views, 1);

    // Between two converged frames, the image (8-bit, after the tonemap) changes by less than 1/255 on average. An
    // edge pixel's samples still change with the jitter, and the blend passes a tenth of that on, so a pixel can change
    // by a few LSB; a rejected history (flicker) would change it by up to 255.
    REQUIRE(before.size() == 1);
    REQUIRE(after.size() == 1);
    double total = 0.0;
    int largest = 0;
    for (std::size_t index = 0; index < before[0].pixels.size(); ++index)
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            const int difference = std::abs(
                static_cast<int>(before[0].pixels[index][channel]) - static_cast<int>(after[0].pixels[index][channel]));
            total += difference;
            largest = std::max(largest, difference);
        }
    }
    const double mean = total / static_cast<double>(before[0].pixels.size() * 3);
    INFO("mean change " << mean << " LSB, largest " << largest << " LSB");
    CHECK(mean < 1.0);
    CHECK(largest <= 16);

    // With TAA, the line's edges are intermediate values
    INFO("intermediate pixels " << count_intermediate(converged));
    CHECK(count_intermediate(converged) > 40);
}

TEST_CASE("a cube moving one pixel per frame leaves no ghost trail behind it", "[renderer]")
{
    renderer_fixture fixture{ unit_exposure() };
    // The front face is 4.5 m away; it moves right by one of its pixels per frame
    const float step = pixel_metres(4.5f, fixture.output_size.height);
    const scene::entity cube = fixture.add_unit_cube({ -1.0f, 0.0f, -5.0f }, unlit({ 1.0f, 1.0f, 1.0f, 1.0f }));
    fixture.publish();

    view_description main = taa_view(fixture);
    const rhi::texture_handle hdr = capture(fixture, main, "hdr_color", rhi::format::rgba16_float);
    const rhi::texture_handle resolved = capture(fixture, main, "taa_output", rhi::format::rgba16_float);
    const std::vector views{ main };
    fixture.render_and_read(views);
    const auto left_edge = [&](const hdr_image& image)
    {
        const std::uint32_t y = image.height / 2;
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            if (image.at(x, y).x() > 0.5f)
            {
                return x;
            }
        }
        return image.width;
    };
    const std::uint32_t start = left_edge(read_hdr(fixture, hdr));

    constexpr std::uint32_t frames = 16;
    for (std::uint32_t frame = 0; frame < frames; ++frame)
    {
        scene::transform local = fixture.world.local(cube);
        local.position.x() += step;
        fixture.world.set_local(cube, local);
        fixture.publish();
        fixture.render_and_read(views);
    }
    const hdr_image current = read_hdr(fixture, hdr);
    const hdr_image output = read_hdr(fixture, resolved);
    const std::uint32_t left = left_edge(current);
    INFO("left edge from " << start << " to " << left);
    REQUIRE(left > start + frames - 2);
    REQUIRE(left < start + frames + 2);

    // Behind the cube, past four pixels (frames) of its trailing edge, the background is back
    const std::uint32_t y = current.height / 2;
    float worst = 0.0f;
    for (std::uint32_t x = start; x + 4 < left; ++x)
    {
        for (std::uint32_t row = y - 4; row <= y + 4; ++row)
        {
            worst = std::max(worst, output.at(x, row).maxCoeff());
        }
    }
    INFO("brightest pixel behind the cube " << worst);
    CHECK(worst <= 0.02f);
    // The cube itself stays white
    CHECK(output.at(left + 4, y).x() >= 0.95f);
}

TEST_CASE("a TAA history reset gives the unaccumulated, jittered frame", "[renderer]")
{
    renderer_fixture fixture{ unit_exposure() };
    add_diagonal_line(fixture);
    fixture.publish();

    view_description main = taa_view(fixture);
    const rhi::texture_handle hdr = capture(fixture, main, "hdr_color", rhi::format::rgba16_float);
    const rhi::texture_handle resolved = capture(fixture, main, "taa_output", rhi::format::rgba16_float);
    std::vector views{ main };
    fixture.render_and_read(views);
    const hdr_image first = read_hdr(fixture, hdr);
    // The first frame has no history: its output is its colour
    CHECK(read_hdr(fixture, resolved).pixels == first.pixels);

    fixture.render_and_read(views, 8);
    const hdr_image accumulated = read_hdr(fixture, resolved);
    CHECK(accumulated.pixels != read_hdr(fixture, hdr).pixels);

    views[0].options.reset_history = true;
    fixture.render_and_read(views);
    const hdr_image reset = read_hdr(fixture, hdr);
    // The jitter starts again at its first sample, and nothing is accumulated
    CHECK(reset.pixels == first.pixels);
    CHECK(read_hdr(fixture, resolved).pixels == reset.pixels);
}

TEST_CASE("with TAA disabled, taa_output is hdr_color without jitter", "[renderer]")
{
    renderer_fixture_options options = unit_exposure();
    options.settings.taa.enabled = false;
    renderer_fixture fixture{ options };
    add_diagonal_line(fixture);
    fixture.publish();

    view_description main = taa_view(fixture);
    const rhi::texture_handle hdr = capture(fixture, main, "hdr_color", rhi::format::rgba16_float);
    const rhi::texture_handle resolved = capture(fixture, main, "taa_output", rhi::format::rgba16_float);
    const std::vector views{ main };
    fixture.render_and_read(views, 3);
    const hdr_image color = read_hdr(fixture, hdr);
    CHECK(read_hdr(fixture, resolved).pixels == color.pixels);
    CHECK(count_intermediate(color) == 0);
}
