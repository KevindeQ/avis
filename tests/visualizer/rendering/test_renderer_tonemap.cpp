// Exposure and tonemapping (docs/tasks/11-renderer.md §11.16): a manual EV100 pre-exposes a surface of known luminance
// as expected, automatic exposure converges on a uniform grey to the key and brightens faster than it darkens, the
// ACES and AgX curves and the sRGB encoding match CPU references, and the dither keeps a flat region's mean. On the
// renderer fixture, with the debug layer and GPU-based validation.

#include "helpers/renderer_fixture.h"
#include "helpers/tonemap_reference.h"
#include "rendering/passes/exposure_passes.h"
#include "rendering/passes/lighting_passes.h"
#include "rendering/view.h"

#include "avis/graphics/readback_data.h"
#include "avis/rhi/formats.h"

#include <Eigen/Core>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
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

    // The RGB of every texel of an RGBA16F image, row-major
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

    float luminance(const Eigen::Vector3f& color)
    {
        return color.dot(Eigen::Vector3f{ 0.2126f, 0.7152f, 0.0722f });
    }

    // The header of the view's exposure buffer (shared_types.hlsli)
    struct exposure_header
    {
        float current = 0.0f;
        float previous = 0.0f;
        float luminance = 0.0f;
    };

    exposure_header read_exposure(renderer_fixture& fixture, const rhi::buffer_handle buffer)
    {
        exposure_header header;
        const std::span<const std::byte> bytes = fixture.mapped_bytes(buffer);
        std::memcpy(&header.current, bytes.data() + gpu::exposure_offset_current, 4);
        std::memcpy(&header.previous, bytes.data() + gpu::exposure_offset_previous, 4);
        std::memcpy(&header.luminance, bytes.data() + gpu::exposure_offset_luminance, 4);
        return header;
    }

    renderer_fixture_options exposure_options(const exposure_mode mode, const float ev100)
    {
        renderer_fixture_options options;
        options.settings.exposure.mode = mode;
        options.settings.exposure.ev100 = ev100;
        return options;
    }

    // Manual exposure with a pre-exposure of 1: 1 / (1.2 · 2^EV100) = 1
    constexpr float unit_ev100 = -0.2630344f; // −log2(1.2)

    // A view without TAA or GTAO, under a uniform sky of `radiance` (cd/m²): the sky is the whole image where nothing
    // is drawn
    view_description plain_view(renderer_fixture& fixture, const float radiance)
    {
        fixture.renderer->set_environment_override(Eigen::Vector3f::Constant(radiance));
        ambient_occlusion_settings no_ao = fixture.renderer->ao_parameters();
        no_ao.intensity = 0.0f;
        fixture.renderer->set_ao_parameters(no_ao);
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

    rhi::buffer_handle capture_exposure(renderer_fixture& fixture, view_description& view)
    {
        const rhi::buffer_handle buffer = fixture.make_readback_buffer(gpu::exposure_buffer_bytes);
        view.options.buffer_captures.push_back({ .resource = "exposure", .buffer = buffer });
        return buffer;
    }

    // A black lit material emitting `radiance` (cd/m²): under a black sky and no lights, it shows its emission only
    scene::material_description emitter(const Eigen::Vector3f& radiance)
    {
        scene::material_description material;
        material.base_color_factor = { 0.0f, 0.0f, 0.0f, 1.0f };
        material.metallic_factor = 0.0f;
        material.roughness_factor = 1.0f;
        material.emissive_factor = radiance;
        return material;
    }

    // `frames` frames, the wall time advancing by `step` before each, so the adaptation sees that frame time
    std::vector<tests::rgba8_image> render_timed(
        renderer_fixture& fixture,
        const std::vector<view_description>& views,
        const int frames,
        const std::chrono::steady_clock::duration step)
    {
        std::vector<tests::rgba8_image> images;
        for (int frame = 0; frame < frames; ++frame)
        {
            fixture.wall_now = fixture.wall_now.value_or(std::chrono::steady_clock::time_point{}) + step;
            images = fixture.render_and_read(views);
        }
        return images;
    }

    constexpr std::chrono::steady_clock::duration thirtieth =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>{ 1.0 / 30.0 });

    // How much of a step of `stops` is left, from the exposure that brings `radiance` to the key
    double remaining(const exposure_header& header, const float radiance, const double stops)
    {
        return std::abs(std::log2(static_cast<double>(header.current) * radiance / exposure_key)) / stops;
    }
} // namespace

TEST_CASE(
    "a manual EV100 pre-exposes a surface of known luminance, and its change shows in the same frame", "[renderer]")
{
    renderer_fixture fixture{ exposure_options(exposure_mode::manual, 10.0f) };
    // 1024 cd/m² is exact in R11G11B10, so the G-buffer keeps it; at EV100 10 it pre-exposes to 1 / 1.2
    fixture.add_lit_quad({ 0.0f, 0.0f, -5.0f }, 4.0f, 4.0f, emitter(Eigen::Vector3f::Constant(1024.0f)));
    fixture.publish();

    view_description main = plain_view(fixture, 0.0f);
    const rhi::texture_handle exposed = capture(fixture, main, "taa_output", rhi::format::rgba16_float);
    const rhi::buffer_handle exposure = capture_exposure(fixture, main);
    std::vector views{ main };
    std::vector images = fixture.render_and_read(views);
    REQUIRE(images.size() == 1);

    const std::uint32_t x = fixture.output_size.width / 2;
    const std::uint32_t y = fixture.output_size.height / 2;
    const float expected = 1024.0f * manual_exposure(10.0f);
    CHECK(expected == Catch::Approx(1.0f / 1.2f));
    const hdr_image first = read_hdr(fixture, exposed);
    for (int channel = 0; channel < 3; ++channel)
    {
        CHECK(first.at(x, y)[channel] == Catch::Approx(expected).epsilon(1.0e-3));
    }
    CHECK(first.at(1, 1).isZero()); // the black sky
    const exposure_header header = read_exposure(fixture, exposure);
    CHECK(header.current == Catch::Approx(manual_exposure(10.0f)));
    CHECK(header.previous == Catch::Approx(manual_exposure(10.0f)));
    CHECK(header.luminance == 0.0f); // nothing measured in manual mode
    const Eigen::Vector3d lsb =
        tests::tonemapped_lsb(first.at(x, y).cast<double>(), fixture.renderer->settings().tonemapper);
    CHECK(std::abs(static_cast<double>(images[0].at(x, y)[0]) - lsb.x()) <= 1.0);

    // One EV more halves the exposure. The frame that makes the change is pre-exposed with the old exposure, and its
    // tonemap rescales to the new one; the next frame is pre-exposed with the new one.
    exposure_settings darker = fixture.renderer->exposure_parameters();
    darker.ev100 = 11.0f;
    fixture.renderer->set_exposure_parameters(darker);
    images = fixture.render_and_read(views);
    const hdr_image changing = read_hdr(fixture, exposed);
    CHECK(changing.at(x, y).x() == Catch::Approx(expected).epsilon(1.0e-3));
    const exposure_header changed = read_exposure(fixture, exposure);
    CHECK(changed.current == Catch::Approx(manual_exposure(11.0f)));
    CHECK(changed.previous == Catch::Approx(manual_exposure(10.0f)));
    const Eigen::Vector3d halved = tests::tonemapped_lsb(
        Eigen::Vector3d::Constant(0.5 * changing.at(x, y).x()), fixture.renderer->settings().tonemapper);
    CHECK(std::abs(static_cast<double>(images[0].at(x, y)[0]) - halved.x()) <= 1.0);

    fixture.render_and_read(views);
    CHECK(read_hdr(fixture, exposed).at(x, y).x() == Catch::Approx(0.5f * expected).epsilon(1.0e-3));
}

TEST_CASE(
    "automatic exposure converges on a uniform grey to the key, and brightens faster than it darkens", "[renderer]")
{
    // From the default EV100 12, a sky of 100 cd/m² is 3.15 stops below the key: the exposure rises to it
    constexpr float grey = 100.0f;
    renderer_fixture fixture{ exposure_options(exposure_mode::automatic, 12.0f) };
    fixture.publish();
    view_description main = plain_view(fixture, grey);
    main.options.taa = true; // its history is rescaled as the exposure changes
    const rhi::texture_handle exposed = capture(fixture, main, "taa_output", rhi::format::rgba16_float);
    const rhi::buffer_handle exposure = capture_exposure(fixture, main);
    const std::vector views{ main };

    render_timed(fixture, views, 60, thirtieth); // 2 s
    const hdr_image converged = read_hdr(fixture, exposed);
    double total = 0.0;
    for (const Eigen::Vector3f& pixel : converged.pixels)
    {
        total += luminance(pixel);
    }
    const double mean = total / static_cast<double>(converged.pixels.size());
    INFO("mean pre-exposed luminance " << mean);
    CHECK(std::abs(mean / exposure_key - 1.0) < 0.05);
    CHECK(std::abs(luminance(converged.at(0, 0)) - luminance(converged.at(64, 36))) < 1.0e-3f); // uniform
    const exposure_header header = read_exposure(fixture, exposure);
    CHECK(header.luminance == Catch::Approx(grey).epsilon(0.01)); // the histogram measures the sky's luminance
    CHECK(std::abs(header.current * grey / exposure_key - 1.0f) < 0.05f);

    // A step of 4 stops either way, for 10 frames (1/3 s): rising, the exposure covers more of it than falling
    constexpr double stops = 4.0;
    SECTION("brightening")
    {
        constexpr float darker_sky = grey / 16.0f;
        fixture.renderer->set_environment_override(Eigen::Vector3f::Constant(darker_sky));
        render_timed(fixture, views, 10, thirtieth);
        const double left = remaining(read_exposure(fixture, exposure), darker_sky, stops);
        INFO("left of the step " << left);
        // exp(−3 · 1/3) = 0.37
        CHECK(left > 0.30);
        CHECK(left < 0.45);
    }
    SECTION("darkening")
    {
        constexpr float brighter_sky = grey * 16.0f;
        fixture.renderer->set_environment_override(Eigen::Vector3f::Constant(brighter_sky));
        render_timed(fixture, views, 10, thirtieth);
        const double left = remaining(read_exposure(fixture, exposure), brighter_sky, stops);
        INFO("left of the step " << left);
        // exp(−1 · 1/3) = 0.72
        CHECK(left > 0.65);
        CHECK(left < 0.80);
    }
}

TEST_CASE("the ACES and AgX tonemaps and the sRGB encoding match their CPU references", "[renderer]")
{
    const tonemap_operator tonemapper = GENERATE(tonemap_operator::aces, tonemap_operator::agx);
    renderer_fixture_options options = exposure_options(exposure_mode::manual, unit_ev100);
    options.settings.tonemapper = tonemapper;
    renderer_fixture fixture{ options };
    INFO("tonemapper " << to_string(tonemapper));

    // 16 emitters in a 4 × 4 grid at 5 m, 1 m wide (12 pixels), from 1/16 to 11 cd/m² by half stops, with tints
    // cycling through grey and three colours, so the matrices of both curves matter
    const std::array tints{ Eigen::Vector3f{ 1.0f, 1.0f, 1.0f },
                            Eigen::Vector3f{ 1.0f, 0.5f, 0.25f },
                            Eigen::Vector3f{ 0.2f, 0.6f, 1.0f },
                            Eigen::Vector3f{ 0.3f, 1.0f, 0.4f } };
    for (int index = 0; index < 16; ++index)
    {
        const float radiance = std::exp2(0.5f * static_cast<float>(index) - 4.0f);
        const float x = -2.25f + 1.5f * static_cast<float>(index % 4);
        const float y = -2.25f + 1.5f * static_cast<float>(index / 4);
        fixture.add_lit_quad({ x, y, -5.0f }, 1.0f, 1.0f, emitter(radiance * tints[index % 4]));
    }
    fixture.publish();

    view_description main = plain_view(fixture, 0.0f);
    const rhi::texture_handle exposed = capture(fixture, main, "taa_output", rhi::format::rgba16_float);
    const std::vector views{ main };
    const std::vector images = fixture.render_and_read(views);
    REQUIRE(images.size() == 1);
    const hdr_image input = read_hdr(fixture, exposed);

    // Every pixel: the tonemap of what it maps (the exposure change is 1), within the dither and rounding's 1 LSB
    std::set<float> luminances;
    double largest = 0.0;
    for (std::uint32_t y = 0; y < input.height; ++y)
    {
        for (std::uint32_t x = 0; x < input.width; ++x)
        {
            const Eigen::Vector3f& pixel = input.at(x, y);
            if (luminance(pixel) > 0.0f)
            {
                luminances.insert(luminance(pixel));
            }
            const Eigen::Vector3d expected = tests::tonemapped_lsb(pixel.cast<double>(), tonemapper);
            for (int channel = 0; channel < 3; ++channel)
            {
                largest =
                    std::max(largest, std::abs(static_cast<double>(images[0].at(x, y)[channel]) - expected[channel]));
            }
        }
    }
    INFO("largest difference " << largest << " LSB");
    CHECK(luminances.size() >= 16);
    CHECK(largest <= 1.0 + 1.0e-3);
}

TEST_CASE("the dither doesn't change the mean of a flat region by more than 0.25 LSB", "[renderer]")
{
    const tonemap_operator tonemapper = GENERATE(tonemap_operator::aces, tonemap_operator::agx);
    renderer_fixture_options options = exposure_options(exposure_mode::manual, unit_ev100);
    options.settings.tonemapper = tonemapper;
    renderer_fixture fixture{ options };
    fixture.publish();
    INFO("tonemapper " << to_string(tonemapper));

    // A grey whose 8-bit value is about halfway between two levels: without the dither every pixel would round
    // the same way, and the mean would be off by about half an LSB
    float grey = 0.05f;
    for (; grey < 1.0f; grey += 0.0005f)
    {
        const double value = tests::tonemapped_lsb(Eigen::Vector3d::Constant(grey), tonemapper).x();
        const double fraction = value - std::floor(value);
        if (fraction > 0.4 && fraction < 0.6)
        {
            break;
        }
    }
    view_description main = plain_view(fixture, grey);
    const rhi::texture_handle exposed = capture(fixture, main, "taa_output", rhi::format::rgba16_float);
    const std::vector views{ main };
    const std::vector images = fixture.render_and_read(views);
    REQUIRE(images.size() == 1);

    const Eigen::Vector3f input = read_hdr(fixture, exposed).at(0, 0);
    const double expected = tests::tonemapped_lsb(input.cast<double>(), tonemapper).x();
    INFO("the grey's 8-bit value " << expected);
    double total = 0.0;
    std::set<int> levels;
    for (std::uint32_t y = 0; y < images[0].height; ++y)
    {
        for (std::uint32_t x = 0; x < images[0].width; ++x)
        {
            const int value = images[0].at(x, y)[0];
            total += value;
            levels.insert(value);
            CHECK(std::abs(static_cast<double>(value) - expected) <= 1.0);
        }
    }
    const double mean = total / (static_cast<double>(images[0].width) * images[0].height);
    INFO("mean " << mean);
    CHECK(std::abs(mean - expected) <= 0.25);
    CHECK(levels.size() == 2); // the two levels around the value
}
