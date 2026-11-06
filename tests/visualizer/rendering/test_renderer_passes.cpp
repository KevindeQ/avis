// The depth prepass, linearize depth and the G-buffer (docs/tasks/11-renderer.md §11.10): depth and normals of a cube,
// velocity, points and lines, mask materials and the depth_mips chain, read back through resource captures. Plus the
// CPU side: the TAA jitter, the jittered projection and the depth_mips reduction spans. On the renderer fixture, with
// the debug layer and GPU-based validation.

#include "helpers/renderer_fixture.h"
#include "rendering/passes/depth_passes.h"
#include "rendering/view.h"
#include "rendering/view_constants.h"

#include "avis/graphics/readback_data.h"
#include "avis/rhi/formats.h"
#include "avis/scene/assets/mesh_builder.h"

#include <Eigen/Core>

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
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::renderer_fixture;
using avis::tests::renderer_fixture_options;

namespace
{
    // ---- reading captured texels ----

    // One float of an R32F (components = 1) or RG32F (components = 2) texel
    float float_at(
        const graphics::readback_image& image,
        const std::uint32_t x,
        const std::uint32_t y,
        const std::uint32_t components = 1,
        const std::uint32_t component = 0)
    {
        float value = 0.0f;
        const std::size_t offset = static_cast<std::size_t>(y) * image.row_pitch() +
                                   (static_cast<std::size_t>(x) * components + component) * sizeof(float);
        std::memcpy(&value, image.bytes().data() + offset, sizeof(value));
        return value;
    }

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

    // One component of an RG16_FLOAT texel
    float half_at(
        const graphics::readback_image& image, const std::uint32_t x, const std::uint32_t y, const int component)
    {
        std::uint16_t value = 0;
        const std::size_t offset =
            static_cast<std::size_t>(y) * image.row_pitch() + static_cast<std::size_t>(x) * 4 + component * 2;
        std::memcpy(&value, image.bytes().data() + offset, sizeof(value));
        return half_to_float(value);
    }

    // An RG16_UNORM octahedral normal, decoded as packing.hlsli does
    Eigen::Vector3f normal_at(const graphics::readback_image& image, const std::uint32_t x, const std::uint32_t y)
    {
        std::array<std::uint16_t, 2> encoded{};
        std::memcpy(
            encoded.data(),
            image.bytes().data() + static_cast<std::size_t>(y) * image.row_pitch() + static_cast<std::size_t>(x) * 4,
            sizeof(encoded));
        return scene::decode_octahedral(encoded);
    }

    // ---- scenes and frames ----

    // A view of the fixture's output without the TAA jitter, so pixel centres sample exactly where the test expects
    view_description steady_view(renderer_fixture& fixture)
    {
        view_description made = fixture.view();
        made.options.taa = false;
        return made;
    }

    // Adds a capture of `resource` (mip `mip`) to `view`, returning the texture it lands in
    rhi::texture_handle capture(
        renderer_fixture& fixture,
        view_description& view,
        const std::string& resource,
        const rhi::format format,
        const std::uint32_t mip = 0)
    {
        const graphics::texture_extent size{ std::max(view.size.width >> mip, 1u),
                                             std::max(view.size.height >> mip, 1u) };
        const rhi::texture_handle texture = fixture.make_texture(format, size);
        view.options.captures.push_back({ .resource = resource, .mip = mip, .texture = texture });
        return texture;
    }

    // One pixel at view depth `depth` for the fixture's 60° vertical field of view, in metres
    float pixel_metres(const float depth, const std::uint32_t height)
    {
        return 2.0f * depth * std::tan(0.5f * camera{}.vertical_field_of_view) / static_cast<float>(height);
    }

    std::size_t count_finite(const graphics::readback_image& linear_depth)
    {
        std::size_t count = 0;
        for (std::uint32_t y = 0; y < linear_depth.height(); ++y)
        {
            for (std::uint32_t x = 0; x < linear_depth.width(); ++x)
            {
                count += std::isfinite(float_at(linear_depth, x, y)) ? 1 : 0;
            }
        }
        return count;
    }
} // namespace

// ---- CPU ----

TEST_CASE("the TAA jitter is Halton(2, 3) over 16 distinct sub-pixel samples", "[renderer][unit]")
{
    CHECK(halton(1, 2) == Catch::Approx(0.5f));
    CHECK(halton(2, 2) == Catch::Approx(0.25f));
    CHECK(halton(3, 2) == Catch::Approx(0.75f));
    CHECK(halton(1, 3) == Catch::Approx(1.0f / 3.0f));
    CHECK(halton(2, 3) == Catch::Approx(2.0f / 3.0f));
    CHECK(halton(3, 3) == Catch::Approx(1.0f / 9.0f)); // 3 is "10" in base 3
    CHECK(halton(4, 3) == Catch::Approx(4.0f / 9.0f)); // "11": 1/3 + 1/9

    std::set<std::pair<float, float>> samples;
    for (std::uint64_t sequence = 0; sequence < jitter_sample_count; ++sequence)
    {
        const Eigen::Vector2f jitter = jitter_pixels(sequence);
        CHECK(jitter.x() >= -0.5f);
        CHECK(jitter.x() < 0.5f);
        CHECK(jitter.y() >= -0.5f);
        CHECK(jitter.y() < 0.5f);
        samples.insert({ jitter.x(), jitter.y() });
        CHECK(jitter_pixels(sequence + jitter_sample_count) == jitter); // wraps
    }
    CHECK(samples.size() == jitter_sample_count);
    CHECK(jitter_pixels(0) == Eigen::Vector2f{ 0.0f, 1.0f / 3.0f - 0.5f });
}

TEST_CASE("a jittered projection moves the image by the jitter in pixels", "[renderer][unit]")
{
    constexpr std::uint32_t width = 128;
    constexpr std::uint32_t height = 72;
    const Eigen::Matrix4f projection = camera{}.projection(static_cast<float>(width) / height);
    const Eigen::Vector2f jitter{ 0.25f, -0.375f };
    const Eigen::Matrix4f jittered = jitter_projection(projection, jitter, width, height);

    const auto pixel = [](const Eigen::Matrix4f& matrix, const Eigen::Vector4f& point)
    {
        const Eigen::Vector4f clip = matrix * point;
        const float x = (clip.x() / clip.w() * 0.5f + 0.5f) * width;
        const float y = (0.5f - clip.y() / clip.w() * 0.5f) * height; // pixel y down
        return Eigen::Vector3f{ x, y, clip.z() / clip.w() };
    };
    for (const Eigen::Vector4f& point : { Eigen::Vector4f{ 0.0f, 0.0f, -5.0f, 1.0f },
                                          Eigen::Vector4f{ 1.0f, -0.5f, -2.0f, 1.0f },
                                          Eigen::Vector4f{ -3.0f, 2.0f, -40.0f, 1.0f } })
    {
        const Eigen::Vector3f plain = pixel(projection, point);
        const Eigen::Vector3f moved = pixel(jittered, point);
        CHECK(moved.x() - plain.x() == Catch::Approx(jitter.x()).margin(1e-4));
        CHECK(moved.y() - plain.y() == Catch::Approx(jitter.y()).margin(1e-4));
        CHECK(moved.z() == Catch::Approx(plain.z())); // depth unchanged
    }
}

TEST_CASE("depth_mips covers every texel of the mip before", "[renderer][unit]")
{
    CHECK(depth_mip_levels(128, 72) == depth_mip_count);
    CHECK(depth_mip_levels(8, 4) == 4);
    CHECK(depth_mip_levels(1, 1) == 1);

    // 9 → 4: the last texel also reduces source texel 8
    CHECK(depth_mip_span(0, 9).first == 0);
    CHECK(depth_mip_span(0, 9).last == 1);
    CHECK(depth_mip_span(3, 9).first == 6);
    CHECK(depth_mip_span(3, 9).last == 8);
    CHECK(depth_mip_span(0, 1).last == 0); // 1 → 1
    for (const std::uint32_t extent : { 1u, 2u, 3u, 9u, 18u, 37u })
    {
        const std::uint32_t destination = std::max(extent / 2, 1u);
        std::uint32_t next = 0;
        for (std::uint32_t texel = 0; texel < destination; ++texel)
        {
            const reduction_span span = depth_mip_span(texel, extent);
            CHECK(span.first == next); // contiguous, without gaps or overlaps
            next = span.last + 1;
        }
        CHECK(next == extent);
    }
}

// ---- GPU ----

TEST_CASE_METHOD(
    renderer_fixture, "a cube in front of the camera: linear depth, reverse-Z and the normal", "[renderer]")
{
    add_unit_cube({ 0.0f, 0.0f, -5.0f });
    publish();

    view_description main = steady_view(*this);
    const rhi::texture_handle linear_depth = capture(*this, main, "linear_depth", rhi::format::r32_float);
    const rhi::texture_handle depth = capture(*this, main, "depth", rhi::format::d32_float);
    const rhi::texture_handle normal = capture(*this, main, "gbuffer_normal", rhi::format::rg16_unorm);
    const std::vector views{ main };
    const std::vector images = render_and_read(views);

    const std::uint32_t centre_x = output_size.width / 2;
    const std::uint32_t centre_y = output_size.height / 2;
    const graphics::readback_image linear = read_subresource(linear_depth);
    CHECK(float_at(linear, centre_x, centre_y) == Catch::Approx(4.5f).margin(1e-3));
    CHECK(std::isinf(float_at(linear, 0, 0))); // nothing drawn

    const graphics::readback_image device_depth = read_subresource(depth);
    CHECK(float_at(device_depth, 0, 0) == 0.0f); // cleared to the far plane
    // An infinite reverse-Z projection: depth = near / distance
    CHECK(float_at(device_depth, centre_x, centre_y) == Catch::Approx(0.1f / 4.5f).epsilon(1e-4));

    const Eigen::Vector3f face_normal = normal_at(read_subresource(normal), centre_x, centre_y);
    CHECK(face_normal.z() == Catch::Approx(1.0f).margin(1e-3));
    CHECK(std::abs(face_normal.x()) < 1e-3f);
    CHECK(std::abs(face_normal.y()) < 1e-3f);

    // The lit cube stands out from the ground of the default environment around it (11.11)
    REQUIRE(images.size() == 1);
    CHECK(images[0].at(centre_x, centre_y) != images[0].at(0, 0));
}

TEST_CASE_METHOD(
    renderer_fixture, "velocity is zero when nothing moves and one pixel for a one-pixel pan", "[renderer]")
{
    // A wall filling the view at 10 m
    constexpr float distance = 10.0f;
    add_unlit_quad({ 0.0f, 0.0f, -distance }, 100.0f, 100.0f, { 0.5f, 0.5f, 0.5f, 1.0f });
    publish();

    view_description main = view(); // TAA on: velocity is unjittered
    const rhi::texture_handle velocity = capture(*this, main, "velocity", rhi::format::rg16_float);

    const auto check_velocity = [&](const float expected_x_pixels)
    {
        const graphics::readback_image image = read_subresource(velocity);
        float worst = 0.0f;
        for (std::uint32_t y = 0; y < image.height(); ++y)
        {
            for (std::uint32_t x = 0; x < image.width(); ++x)
            {
                const float x_pixels = half_at(image, x, y, 0) * static_cast<float>(image.width());
                const float y_pixels = half_at(image, x, y, 1) * static_cast<float>(image.height());
                worst = std::max({ worst, std::abs(x_pixels - expected_x_pixels), std::abs(y_pixels) });
            }
        }
        INFO("expected " << expected_x_pixels << " px, worst error " << worst << " px");
        CHECK(worst <= 0.05f);
    };

    {
        const std::vector views{ main };
        render_and_read(views, 2); // the second frame has a previous one
        check_velocity(0.0f);
    }

    // The camera moves right by one pixel at the wall: the wall moves left on screen
    main.camera.world_from_view.translation().x() += pixel_metres(distance, output_size.height);
    {
        const std::vector views{ main };
        render_and_read(views, 1);
        check_velocity(-1.0f);
    }
}

TEST_CASE("1,000 points of 0.1 m at 10 m cover the expected pixels", "[renderer]")
{
    renderer_fixture_options options;
    options.output_size = { 1280, 720 };
    renderer_fixture fixture{ options };

    // A 40 × 25 grid 0.4 m apart, facing the camera
    constexpr float distance = 10.0f;
    constexpr float size = 0.1f;
    scene::mesh_builder builder{ scene::mesh_topology::points };
    for (int row = 0; row < 25; ++row)
    {
        for (int column = 0; column < 40; ++column)
        {
            builder.add_vertex(
                { (static_cast<float>(column) - 19.5f) * 0.4f, (static_cast<float>(row) - 12.0f) * 0.4f, 0.0f });
        }
    }
    result<scene::mesh_data> points = std::move(builder).build();
    REQUIRE(points.has_value());
    scene::material_description material;
    material.point_size = size;
    fixture.add_mesh("points", { 0.0f, 0.0f, -distance }, std::move(*points), material);
    fixture.publish();

    view_description main = steady_view(fixture);
    const rhi::texture_handle linear_depth = capture(fixture, main, "linear_depth", rhi::format::r32_float);
    const std::vector views{ main };
    fixture.render_and_read(views);

    // Each point is a disc of point_size at its depth: its radius in pixels is 0.5 · size / pixel_metres
    const float radius = 0.5f * size / pixel_metres(distance, options.output_size.height);
    const double expected = 1000.0 * std::numbers::pi * radius * radius;
    const std::size_t covered = count_finite(fixture.read_subresource(linear_depth));
    INFO("covered " << covered << " pixels, expected " << expected);
    CHECK(std::abs(static_cast<double>(covered) - expected) <= 0.1 * expected);
}

TEST_CASE_METHOD(renderer_fixture, "a 3-pixel line covers 3 pixels across its centre", "[renderer]")
{
    // A horizontal line through the centre of pixel row height / 2, wider than the view
    constexpr float distance = 5.0f;
    const float pixel = pixel_metres(distance, output_size.height);
    scene::mesh_builder builder{ scene::mesh_topology::lines };
    builder.add_vertex({ -20.0f, -0.5f * pixel, 0.0f });
    builder.add_vertex({ 20.0f, -0.5f * pixel, 0.0f });
    result<scene::mesh_data> line = std::move(builder).build();
    REQUIRE(line.has_value());
    scene::material_description material;
    material.line_width = 3.0f;
    add_mesh("line", { 0.0f, 0.0f, -distance }, std::move(*line), material);
    publish();

    view_description main = steady_view(*this);
    const rhi::texture_handle linear_depth = capture(*this, main, "linear_depth", rhi::format::r32_float);
    const std::vector views{ main };
    render_and_read(views);

    const graphics::readback_image image = read_subresource(linear_depth);
    for (const std::uint32_t x : { 8u, output_size.width / 2, output_size.width - 8 })
    {
        std::vector<std::uint32_t> rows;
        for (std::uint32_t y = 0; y < image.height(); ++y)
        {
            if (std::isfinite(float_at(image, x, y)))
            {
                rows.push_back(y);
            }
        }
        INFO("column " << x);
        REQUIRE(rows.size() == 3);
        CHECK(rows.front() == output_size.height / 2 - 1);
        CHECK(rows.back() == output_size.height / 2 + 1);
        CHECK(float_at(image, x, output_size.height / 2) == Catch::Approx(distance).margin(1e-3));
    }
}

TEST_CASE_METHOD(renderer_fixture, "mask materials clip below alpha_cutoff", "[renderer]")
{
    scene::material_description below;
    below.alpha = scene::alpha_mode::mask;
    below.alpha_cutoff = 0.5f;
    below.base_color_factor = { 1.0f, 0.0f, 0.0f, 0.3f };
    scene::material_description above = below;
    above.base_color_factor.alpha = 0.7f;
    add_unit_cube({ -1.0f, 0.0f, -5.0f }, below);
    add_unit_cube({ 1.0f, 0.0f, -5.0f }, above);
    publish();

    view_description main = steady_view(*this);
    const rhi::texture_handle linear_depth = capture(*this, main, "linear_depth", rhi::format::r32_float);
    const rhi::texture_handle albedo = capture(*this, main, "gbuffer_albedo", rhi::format::rgba8_unorm_srgb);
    const std::vector views{ main };
    render_and_read(views);

    // The cubes' centres: x = ±1 m at 4.5 m
    const float offset = 1.0f / pixel_metres(4.5f, output_size.height);
    const auto left = static_cast<std::uint32_t>(static_cast<float>(output_size.width) / 2.0f - offset);
    const auto right = static_cast<std::uint32_t>(static_cast<float>(output_size.width) / 2.0f + offset);
    const std::uint32_t y = output_size.height / 2;

    const graphics::readback_image linear = read_subresource(linear_depth);
    CHECK(std::isinf(float_at(linear, left, y))); // clipped in the prepass: no depth, so no G-buffer either
    CHECK(float_at(linear, right, y) == Catch::Approx(4.5f).margin(1e-3));

    const graphics::readback_image colors = read_subresource(albedo);
    const std::byte* kept = colors.bytes().data() + static_cast<std::size_t>(y) * colors.row_pitch() + right * 4;
    CHECK(std::to_integer<int>(kept[0]) == 255);
    CHECK(std::to_integer<int>(kept[1]) == 0);
    const std::byte* cut = colors.bytes().data() + static_cast<std::size_t>(y) * colors.row_pitch() + left * 4;
    CHECK(std::to_integer<int>(cut[0]) == 0); // cleared
}

TEST_CASE_METHOD(renderer_fixture, "the depth_mips chain matches a CPU min/max reduction of linear_depth", "[renderer]")
{
    add_unit_cube({ 0.0f, 0.0f, -5.0f });
    add_unit_cube({ 1.3f, 0.4f, -3.0f });
    add_sphere({ -1.5f, -0.6f, -7.0f }, 1.2f);
    add_ground_plane(40.0f);
    publish();

    view_description main = steady_view(*this);
    main.camera.world_from_view.translation().y() = 1.0f;
    const rhi::texture_handle linear_depth = capture(*this, main, "linear_depth", rhi::format::r32_float);
    const std::uint32_t mips = depth_mip_levels(main.size.width, main.size.height);
    std::vector<rhi::texture_handle> chain;
    for (std::uint32_t mip = 0; mip < mips; ++mip)
    {
        chain.push_back(capture(*this, main, "depth_mips", rhi::format::rg32_float, mip));
    }
    const std::vector views{ main };
    render_and_read(views);

    // The CPU reduction, mip by mip, from linear_depth
    const graphics::readback_image linear = read_subresource(linear_depth);
    std::uint32_t width = linear.width();
    std::uint32_t height = linear.height();
    std::vector<std::pair<float, float>> expected(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const float value = float_at(linear, x, y);
            expected[static_cast<std::size_t>(y) * width + x] = { value, value };
        }
    }
    std::size_t finite_tiles = 0;
    std::size_t empty_tiles = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip)
    {
        if (mip > 0)
        {
            const std::uint32_t next_width = std::max(width / 2, 1u);
            const std::uint32_t next_height = std::max(height / 2, 1u);
            std::vector<std::pair<float, float>> reduced(static_cast<std::size_t>(next_width) * next_height);
            for (std::uint32_t y = 0; y < next_height; ++y)
            {
                const reduction_span rows = depth_mip_span(y, height);
                for (std::uint32_t x = 0; x < next_width; ++x)
                {
                    const reduction_span columns = depth_mip_span(x, width);
                    std::pair<float, float> value{ std::numeric_limits<float>::infinity(),
                                                   -std::numeric_limits<float>::infinity() };
                    for (std::uint32_t row = rows.first; row <= rows.last; ++row)
                    {
                        for (std::uint32_t column = columns.first; column <= columns.last; ++column)
                        {
                            const auto& source = expected[static_cast<std::size_t>(row) * width + column];
                            value.first = std::min(value.first, source.first);
                            value.second = std::max(value.second, source.second);
                        }
                    }
                    reduced[static_cast<std::size_t>(y) * next_width + x] = value;
                }
            }
            expected = std::move(reduced);
            width = next_width;
            height = next_height;
        }

        const graphics::readback_image image = read_subresource(chain[mip]);
        REQUIRE(image.width() == width);
        REQUIRE(image.height() == height);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const auto& value = expected[static_cast<std::size_t>(y) * width + x];
                INFO("mip " << mip << " texel " << x << ", " << y);
                REQUIRE(float_at(image, x, y, 2, 0) == value.first);
                REQUIRE(float_at(image, x, y, 2, 1) == value.second);
                if (mip == mips - 1)
                {
                    (std::isinf(value.first) ? empty_tiles : finite_tiles) += 1;
                }
            }
        }
    }
    CHECK(finite_tiles > 0); // the scene reaches the last mip
    CHECK(empty_tiles > 0); // and so does the sky above the ground plane
}

TEST_CASE_METHOD(
    renderer_fixture, "a capture of an unknown resource or of the wrong format fails the frame", "[renderer]")
{
    publish();
    view_description unknown = steady_view(*this);
    unknown.options.captures.push_back(
        { .resource = "no_such_texture", .texture = make_texture(rhi::format::r32_float, output_size) });
    const std::vector unknown_views{ unknown };
    const result<graphics::submitted_frame> failed = render_frame(unknown_views);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message.find("no_such_texture") != std::string::npos);

    view_description wrong = steady_view(*this);
    wrong.options.captures.push_back(
        { .resource = "linear_depth", .texture = make_texture(rhi::format::rg32_float, output_size) });
    const std::vector wrong_views{ wrong };
    const result<graphics::submitted_frame> refused = render_frame(wrong_views);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("r32_float") != std::string::npos);
}
