// The forward transparent pass (docs/tasks/11-renderer.md §11.14): premultiplied blending into hdr_color, back to front
// whatever the submission order, additive and subtractive blending, the depth test against opaque geometry, the
// reactive mask, and lit transparents shaded like deferred lighting. On the renderer fixture, with the debug layer and
// GPU-based validation.

#include "helpers/renderer_fixture.h"
#include "rendering/passes/lighting_passes.h"
#include "rendering/view.h"

#include "avis/graphics/readback_data.h"
#include "avis/rhi/formats.h"

#include <Eigen/Core>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
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

    // The RGB of an RGBA16F texel
    Eigen::Vector3f hdr_at(const graphics::readback_image& image, const std::uint32_t x, const std::uint32_t y)
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
        return value;
    }

    float r8_at(const graphics::readback_image& image, const std::uint32_t x, const std::uint32_t y)
    {
        const auto value =
            static_cast<std::uint8_t>(image.bytes()[static_cast<std::size_t>(y) * image.row_pitch() + x]);
        return static_cast<float>(value) / 255.0f;
    }

    // Manual exposure with a pre-exposure of 1: 1 / (1.2 · 2^EV100) = 1
    renderer_fixture_options unit_exposure()
    {
        renderer_fixture_options options;
        options.settings.exposure.mode = exposure_mode::manual;
        options.settings.exposure.ev100 = -std::log2(1.2f);
        return options;
    }

    // A frame without TAA jitter, under a black sky, without GTAO
    view_description steady_view(renderer_fixture& fixture)
    {
        fixture.renderer->set_environment_override(Eigen::Vector3f::Zero());
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

    scene::material_description
        translucent(const scene::color color, const scene::blend_operation blend = scene::blend_operation::normal)
    {
        scene::material_description material;
        material.base_color_factor = color;
        material.shading = scene::shading_model::unlit;
        material.alpha = scene::alpha_mode::blend;
        material.blend = blend;
        return material;
    }

    scene::material_description unlit(const scene::color color)
    {
        scene::material_description material;
        material.base_color_factor = color;
        material.shading = scene::shading_model::unlit;
        return material;
    }

    // Renders one frame and reads back the captured hdr_color
    graphics::readback_image render_hdr(renderer_fixture& fixture)
    {
        view_description main = steady_view(fixture);
        const rhi::texture_handle hdr = capture(fixture, main, "hdr_color", rhi::format::rgba16_float);
        const std::vector views{ main };
        fixture.render_and_read(views);
        return fixture.read_subresource(hdr);
    }

    // Two half-transparent quads, red behind green, added in either order; the centre pixel, where both are
    Eigen::Vector3f overlapping_quads(const bool back_first)
    {
        renderer_fixture fixture{ unit_exposure() };
        const auto add_back = [&]
        { fixture.add_lit_quad({ 0.0f, 0.0f, -4.0f }, 2.0f, 2.0f, translucent({ 1.0f, 0.0f, 0.0f, 0.5f })); };
        const auto add_front = [&]
        { fixture.add_lit_quad({ 0.0f, 0.0f, -3.0f }, 1.0f, 1.0f, translucent({ 0.0f, 1.0f, 0.0f, 0.5f })); };
        if (back_first)
        {
            add_back();
            add_front();
        }
        else
        {
            add_front();
            add_back();
        }
        fixture.publish();
        const graphics::readback_image image = render_hdr(fixture);
        return hdr_at(image, image.width() / 2, image.height() / 2);
    }
} // namespace

TEST_CASE("a 50% white unlit quad over black gives 0.5", "[renderer]")
{
    renderer_fixture fixture{ unit_exposure() };
    REQUIRE(manual_exposure(fixture.settings.exposure.ev100) == Catch::Approx(1.0f).epsilon(1e-5));
    fixture.add_lit_quad({ 0.0f, 0.0f, -3.0f }, 1.0f, 1.0f, translucent({ 1.0f, 1.0f, 1.0f, 0.5f }));
    fixture.publish();

    view_description main = steady_view(fixture);
    const rhi::texture_handle hdr = capture(fixture, main, "hdr_color", rhi::format::rgba16_float);
    const rhi::texture_handle reactive = capture(fixture, main, "reactive_mask", rhi::format::r8_unorm);
    const std::vector views{ main };
    fixture.render_and_read(views);

    const graphics::readback_image color = fixture.read_subresource(hdr);
    const graphics::readback_image mask = fixture.read_subresource(reactive);
    const std::uint32_t x = color.width() / 2;
    const std::uint32_t y = color.height() / 2;
    const Eigen::Vector3f centre = hdr_at(color, x, y);
    INFO("centre " << centre.transpose());
    CHECK((centre - Eigen::Vector3f::Constant(0.5f)).cwiseAbs().maxCoeff() <= 1e-3f);
    CHECK(hdr_at(color, 2, 2).isZero());
    // The reactive mask: the quad's alpha where it's drawn, 0 elsewhere
    CHECK(r8_at(mask, x, y) == Catch::Approx(0.5f).margin(1.0f / 255.0f));
    CHECK(r8_at(mask, 2, 2) == 0.0f);
}

TEST_CASE("two overlapping translucent quads blend back to front in either submission order", "[renderer]")
{
    // Red at half alpha over black, then green at half alpha over that
    const Eigen::Vector3f expected{ 0.25f, 0.5f, 0.0f };
    const Eigen::Vector3f back_first = overlapping_quads(true);
    const Eigen::Vector3f front_first = overlapping_quads(false);
    INFO("back first " << back_first.transpose() << ", front first " << front_first.transpose());
    CHECK((back_first - expected).cwiseAbs().maxCoeff() <= 1e-3f);
    CHECK(back_first == front_first);
}

TEST_CASE("additive and subtractive transparents add to and take from the background", "[renderer]")
{
    const auto render = [](const scene::blend_operation blend)
    {
        renderer_fixture fixture{ unit_exposure() };
        // An opaque grey wall, and a quad over its middle
        fixture.add_lit_quad({ 0.0f, 0.0f, -5.0f }, 20.0f, 20.0f, unlit({ 0.5f, 0.5f, 0.5f, 1.0f }));
        fixture.add_lit_quad({ 0.0f, 0.0f, -3.0f }, 1.0f, 1.0f, translucent({ 0.2f, 0.1f, 0.05f, 1.0f }, blend));
        fixture.publish();
        const graphics::readback_image image = render_hdr(fixture);
        return std::pair{ hdr_at(image, 2, 2), hdr_at(image, image.width() / 2, image.height() / 2) };
    };
    const Eigen::Vector3f source{ 0.2f, 0.1f, 0.05f };

    const auto [background, added] = render(scene::blend_operation::additive);
    INFO("background " << background.transpose() << ", additive " << added.transpose());
    CHECK((background - Eigen::Vector3f::Constant(0.5f)).cwiseAbs().maxCoeff() <= 0.01f);
    CHECK((added - (background + source)).cwiseAbs().maxCoeff() <= 1e-3f);

    const auto [background_again, subtracted] = render(scene::blend_operation::subtractive);
    INFO("subtractive " << subtracted.transpose());
    CHECK(background_again == background);
    CHECK((subtracted - (background - source)).cwiseAbs().maxCoeff() <= 1e-3f);
}

TEST_CASE("a translucent quad behind an opaque cube is hidden where the cube covers it", "[renderer]")
{
    renderer_fixture fixture{ unit_exposure() };
    fixture.add_unit_cube({ 0.0f, 0.0f, -3.0f }, unlit({ 0.0f, 0.0f, 1.0f, 1.0f }));
    fixture.add_lit_quad({ 0.0f, 0.0f, -5.0f }, 6.0f, 6.0f, translucent({ 1.0f, 0.0f, 0.0f, 0.5f }));
    fixture.publish();
    const graphics::readback_image image = render_hdr(fixture);

    // The cube's front face, 2.5 m away, covers the pixels within about 0.35 of the half-height of the centre
    const std::uint32_t x = image.width() / 2;
    const std::uint32_t y = image.height() / 2;
    const Eigen::Vector3f covered = hdr_at(image, x, y);
    INFO("covered " << covered.transpose());
    CHECK(covered.x() == 0.0f);
    CHECK(covered.z() == Catch::Approx(1.0f).margin(0.01f));
    // Beside the cube, the quad shows over the black sky
    const Eigen::Vector3f beside = hdr_at(image, x + 30, y);
    INFO("beside " << beside.transpose());
    CHECK((beside - Eigen::Vector3f{ 0.5f, 0.0f, 0.0f }).cwiseAbs().maxCoeff() <= 1e-3f);
}

TEST_CASE("a lit transparent at full alpha shades like the same opaque surface", "[renderer]")
{
    // A wall under the sun, a point light in front of it and a uniform environment, drawn opaque (deferred) or blended
    // at alpha 1 (forward), which replaces the black background. Forward has no opaque geometry, so the clusters in
    // front of the sky get the point light only because the frame draws transparents.
    const auto render = [](const scene::alpha_mode alpha)
    {
        renderer_fixture_options options;
        options.settings.exposure.mode = exposure_mode::manual;
        options.settings.exposure.ev100 = 10.0f;
        renderer_fixture fixture{ options };
        scene::material_description material;
        material.base_color_factor = { 0.8f, 0.8f, 0.8f, 1.0f };
        material.metallic_factor = 0.0f;
        material.roughness_factor = 0.6f;
        material.alpha = alpha;
        fixture.add_lit_quad({ 0.0f, 0.0f, -5.0f }, 20.0f, 20.0f, material);
        fixture.add_sun(Eigen::Vector3f{ 0.3f, -1.0f, -1.0f }.normalized(), 20'000.0f);
        fixture.add_point_light({ 0.5f, 0.0f, -4.0f }, 40'000.0f, 3.0f);
        fixture.publish();
        view_description main = steady_view(fixture);
        fixture.renderer->set_environment_override(Eigen::Vector3f::Constant(1'000.0f));
        const rhi::texture_handle hdr = capture(fixture, main, "hdr_color", rhi::format::rgba16_float);
        const std::vector views{ main };
        fixture.render_and_read(views);
        // Copied out: the image's bytes belong to the fixture
        const graphics::readback_image image = fixture.read_subresource(hdr);
        std::vector<Eigen::Vector3f> pixels;
        for (std::uint32_t y = 0; y < image.height(); ++y)
        {
            for (std::uint32_t x = 0; x < image.width(); ++x)
            {
                pixels.push_back(hdr_at(image, x, y));
            }
        }
        return pixels;
    };
    const std::vector<Eigen::Vector3f> deferred = render(scene::alpha_mode::opaque);
    const std::vector<Eigen::Vector3f> forward = render(scene::alpha_mode::blend);
    REQUIRE(deferred.size() == forward.size());

    float worst = 0.0f;
    float brightest = 0.0f;
    float darkest = 1e30f;
    for (std::size_t index = 0; index < deferred.size(); ++index)
    {
        const Eigen::Vector3f& expected = deferred[index];
        worst = std::max(worst, (forward[index] - expected).cwiseAbs().maxCoeff() / expected.maxCoeff());
        brightest = std::max(brightest, expected.x());
        darkest = std::min(darkest, expected.x());
    }
    INFO("worst relative difference " << worst << ", deferred " << darkest << " to " << brightest);
    // The point light makes the wall uneven, so it's in both images
    CHECK(brightest > 1.5f * darkest);
    CHECK(worst <= 0.03f);
}
