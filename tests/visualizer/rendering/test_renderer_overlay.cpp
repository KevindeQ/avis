// The overlay pass and the selection outline (docs/tasks/11-renderer.md §11.17): overlays tested against the scene's
// depth or drawn on top, in their exact unlit colours; screen-aligned meshes sized in pixels at any distance and by the
// device pixel ratio; layer_priority; and the outline of selected instances. On the renderer fixture, with the debug
// layer and GPU-based validation.

#include "helpers/renderer_fixture.h"
#include "helpers/tonemap_reference.h"
#include "rendering/passes/overlay_pass.h"
#include "rendering/view.h"

#include "avis/rhi/formats.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/rendering.h"

#include <Eigen/Core>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::renderer_fixture;
using avis::tests::renderer_fixture_options;
using avis::tests::rgba8;
using avis::tests::rgba8_image;

namespace
{
    // Manual exposure with a pre-exposure of 1, so an unlit surface of radiance 0.5 is visible
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

    scene::material_description unlit(const scene::color color)
    {
        scene::material_description material;
        material.base_color_factor = color;
        material.shading = scene::shading_model::unlit;
        return material;
    }

    void set_render_options(
        renderer_fixture& fixture,
        const scene::entity entity,
        const scene::draw_layer layer,
        const std::uint8_t priority = 0,
        avis::flags<scene::instance_flags> flags = scene::default_instance_flags)
    {
        fixture.world.registry().emplace_or_replace<scene::components::render_options>(
            entity, scene::components::render_options{ .layer = layer, .layer_priority = priority, .flags = flags });
    }

    // A line mesh of one segment from `start` to `end`
    scene::mesh_data line_mesh(const Eigen::Vector3f& start, const Eigen::Vector3f& end)
    {
        scene::mesh_builder builder{ scene::mesh_topology::lines };
        builder.add_vertex(start);
        builder.add_vertex(end);
        result<scene::mesh_data> mesh = std::move(builder).build();
        REQUIRE(mesh.has_value());
        return std::move(*mesh);
    }

    // A width × height rectangle in the XY plane, centred on the origin, facing +Z; in pixels when screen-aligned
    scene::mesh_data rectangle_mesh(const float width, const float height)
    {
        scene::mesh_builder builder{ scene::mesh_topology::triangles };
        builder.add_vertex({ -0.5f * width, -0.5f * height, 0.0f });
        builder.add_vertex({ 0.5f * width, -0.5f * height, 0.0f });
        builder.add_vertex({ 0.5f * width, 0.5f * height, 0.0f });
        builder.add_vertex({ -0.5f * width, 0.5f * height, 0.0f });
        for (const std::uint32_t index : { 0u, 1u, 2u, 0u, 2u, 3u })
        {
            builder.add_index(index);
        }
        result<scene::mesh_data> mesh = std::move(builder).build();
        REQUIRE(mesh.has_value());
        return std::move(*mesh);
    }

    // The 8-bit sRGB value of a linear one, as the GPU's UNORM conversion rounds it
    std::uint8_t srgb8(const float linear)
    {
        return static_cast<std::uint8_t>(std::lround(255.0 * tests::srgb_encode(linear)));
    }

    rgba8 srgb8(const scene::color color)
    {
        return { srgb8(color.red), srgb8(color.green), srgb8(color.blue), 255 };
    }

    bool same_rgb(const rgba8 a, const rgba8 b)
    {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
    }

    bool black(const rgba8 pixel)
    {
        return pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0;
    }

    struct pixel_box
    {
        std::size_t count = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // The pixels of `color`, and the size of their bounding box
    pixel_box find(const rgba8_image& image, const rgba8 color)
    {
        pixel_box box;
        std::uint32_t left = image.width;
        std::uint32_t right = 0;
        std::uint32_t top = image.height;
        std::uint32_t bottom = 0;
        for (std::uint32_t y = 0; y < image.height; ++y)
        {
            for (std::uint32_t x = 0; x < image.width; ++x)
            {
                if (same_rgb(image.at(x, y), color))
                {
                    ++box.count;
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        }
        if (box.count > 0)
        {
            box.width = right - left + 1;
            box.height = bottom - top + 1;
        }
        return box;
    }
} // namespace

TEST_CASE("an overlay line is hidden behind a cube, and drawn over it on top, in its exact colour", "[renderer]")
{
    renderer_fixture fixture{ unit_exposure() };
    const scene::color line_color{ 0.25f, 1.0f, 0.0f, 1.0f };
    fixture.add_unit_cube({ 0.0f, 0.0f, -5.0f }, unlit({ 0.5f, 0.5f, 0.5f, 1.0f }));
    scene::material_description line_material = unlit(line_color);
    line_material.line_width = 3.0f;
    // A horizontal line through the view's centre, 3 m behind the cube's centre
    const scene::entity line = fixture.add_mesh(
        "line", { 0.0f, 0.0f, -8.0f }, line_mesh({ -4.0f, 0.0f, 0.0f }, { 4.0f, 0.0f, 0.0f }), line_material);
    set_render_options(fixture, line, scene::draw_layer::overlay);
    fixture.publish();
    const view_description view = steady_view(fixture);

    const std::uint32_t centre_x = fixture.output_size.width / 2;
    const std::uint32_t row = fixture.output_size.height / 2; // the line's two middle rows are fully covered
    const std::uint32_t beside_x = centre_x - 20; // on the line (±31 px), away from the cube (±7 px)
    const rgba8 expected = srgb8(line_color);

    const rgba8_image tested = fixture.render_and_read({ &view, 1 }).front();
    INFO(
        "beside the cube " << int{ tested.at(beside_x, row)[0] } << " " << int{ tested.at(beside_x, row)[1] } << " "
                           << int{ tested.at(beside_x, row)[2] });
    CHECK(tested.at(beside_x, row) == expected);
    CHECK(tested.at(beside_x, row - 1) == expected);
    // Behind the cube: hidden, the cube's grey shows
    const rgba8 behind = tested.at(centre_x, row);
    CHECK_FALSE(same_rgb(behind, expected));
    CHECK(behind[0] == behind[1]);
    CHECK(behind[0] > 0);

    set_render_options(fixture, line, scene::draw_layer::overlay_on_top);
    fixture.publish();
    const rgba8_image on_top = fixture.render_and_read({ &view, 1 }).front();
    CHECK(on_top.at(centre_x, row) == expected);
    CHECK(on_top.at(beside_x, row) == expected);
}

TEST_CASE("an overlay on the surface it is drawn on stays visible", "[renderer]")
{
    renderer_fixture fixture{ unit_exposure() };
    const scene::color color{ 1.0f, 0.0f, 0.25f, 1.0f };
    // A wall 6 m away, turned 60° about Y, so its depth changes fast across the view; the overlay lies in its plane
    const Eigen::Quaternionf turned{ Eigen::AngleAxisf{ 1.0471976f, Eigen::Vector3f::UnitY() } };
    const scene::entity wall =
        fixture.add_lit_quad({ 0.0f, 0.0f, -6.0f }, 8.0f, 8.0f, unlit({ 0.5f, 0.5f, 0.5f, 1.0f }));
    const scene::entity overlay =
        fixture.add_mesh("overlay", { 0.0f, 0.0f, -6.0f }, rectangle_mesh(2.0f, 2.0f), unlit(color));
    for (const scene::entity entity : { wall, overlay })
    {
        scene::transform local = fixture.world.local(entity);
        local.rotation = turned;
        fixture.world.set_local(entity, local);
    }
    const view_description view = steady_view(fixture);

    // Depth-tested, it covers what it covers without the test: no pixel is lost to the wall's depth
    set_render_options(fixture, overlay, scene::draw_layer::overlay_on_top);
    fixture.publish();
    const pixel_box untested = find(fixture.render_and_read({ &view, 1 }).front(), srgb8(color));
    set_render_options(fixture, overlay, scene::draw_layer::overlay);
    fixture.publish();
    const pixel_box tested = find(fixture.render_and_read({ &view, 1 }).front(), srgb8(color));
    INFO("on top " << untested.count << " pixels, depth-tested " << tested.count);
    CHECK(untested.count > 100);
    CHECK(tested.count == untested.count);
}

TEST_CASE("a screen-aligned quad keeps its size in pixels, scaled by the device pixel ratio", "[renderer]")
{
    renderer_fixture fixture{ unit_exposure() };
    const scene::color color{ 1.0f, 1.0f, 1.0f, 1.0f };
    const scene::entity label =
        fixture.add_mesh("label", { 0.0f, 0.0f, -5.0f }, rectangle_mesh(20.0f, 10.0f), unlit(color));
    set_render_options(
        fixture,
        label,
        scene::draw_layer::overlay,
        0,
        scene::default_instance_flags | scene::instance_flags::screen_aligned);

    const auto measure = [&](const float distance, const float device_pixel_ratio)
    {
        scene::transform local = fixture.world.local(label);
        local.position = { 0.0f, 0.0f, -distance };
        fixture.world.set_local(label, local);
        fixture.publish();
        view_description view = steady_view(fixture);
        view.device_pixel_ratio = device_pixel_ratio;
        return find(fixture.render_and_read({ &view, 1 }).front(), srgb8(color));
    };

    for (const float distance : { 5.0f, 50.0f })
    {
        INFO("at " << distance << " m");
        const pixel_box box = measure(distance, 1.0f);
        CHECK(box.width == 20);
        CHECK(box.height == 10);
        CHECK(box.count == 200);
    }
    const pixel_box doubled = measure(5.0f, 2.0f);
    CHECK(doubled.width == 40);
    CHECK(doubled.height == 20);
    CHECK(doubled.count == 800);
}

TEST_CASE("layer_priority orders overlapping overlay quads", "[renderer]")
{
    renderer_fixture fixture{ unit_exposure() };
    const scene::color red{ 1.0f, 0.0f, 0.0f, 1.0f };
    const scene::color green{ 0.0f, 1.0f, 0.0f, 1.0f };
    // The green quad is farther away: priority, not depth, decides among overlays, which write no depth
    const scene::entity near_quad = fixture.add_unlit_quad({ -0.3f, 0.0f, -4.0f }, 1.0f, 1.0f, red);
    const scene::entity far_quad = fixture.add_unlit_quad({ 0.3f, 0.0f, -4.5f }, 1.0f, 1.0f, green);
    const view_description view = steady_view(fixture);
    const std::uint32_t overlap_x = fixture.output_size.width / 2;
    const std::uint32_t overlap_y = fixture.output_size.height / 2;

    set_render_options(fixture, near_quad, scene::draw_layer::overlay, 1);
    set_render_options(fixture, far_quad, scene::draw_layer::overlay, 2);
    fixture.publish();
    const rgba8_image green_on_top = fixture.render_and_read({ &view, 1 }).front();
    CHECK(green_on_top.at(overlap_x, overlap_y) == srgb8(green));
    CHECK(green_on_top.at(overlap_x - 10, overlap_y) == srgb8(red));

    set_render_options(fixture, near_quad, scene::draw_layer::overlay, 3);
    fixture.publish();
    const rgba8_image red_on_top = fixture.render_and_read({ &view, 1 }).front();
    CHECK(red_on_top.at(overlap_x, overlap_y) == srgb8(red));
    CHECK(red_on_top.at(overlap_x + 10, overlap_y) == srgb8(green));
}

TEST_CASE("a selected cube has an outline; an unselected one doesn't", "[renderer]")
{
    renderer_fixture fixture{ unit_exposure() };
    const scene::entity cube = fixture.add_unit_cube({ 0.0f, 0.0f, -5.0f }, unlit({ 0.5f, 0.5f, 0.5f, 1.0f }));
    fixture.publish();
    view_description view = steady_view(fixture);
    const rgba8 outline = srgb8(default_selection_color);

    // Unselected: no outline, and no selection mask is rendered
    const rgba8_image plain = fixture.render_and_read({ &view, 1 }).front();
    CHECK(find(plain, outline).count == 0);
    {
        view_description captured = view;
        captured.options.captures.push_back(
            { .resource = "selection_mask", .texture = fixture.make_texture(rhi::format::r8_unorm, view.size) });
        CHECK_FALSE(fixture.render_frame({ &captured, 1 }).has_value());
    }

    set_render_options(
        fixture, cube, scene::draw_layer::world, 0, scene::default_instance_flags | scene::instance_flags::selected);
    fixture.publish();
    const rgba8_image selected = fixture.render_and_read({ &view, 1 }).front();

    // The cube's pixels in the unselected image, and their distance to each other pixel
    const auto in_cube = [&](const int x, const int y)
    {
        return x >= 0 && y >= 0 && x < static_cast<int>(plain.width) && y < static_cast<int>(plain.height) &&
               !black(plain.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)));
    };
    std::size_t outlined = 0;
    std::size_t misplaced = 0;
    std::size_t missed = 0;
    for (int y = 0; y < static_cast<int>(plain.height); ++y)
    {
        for (int x = 0; x < static_cast<int>(plain.width); ++x)
        {
            const rgba8 before = plain.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const rgba8 after = selected.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            if (in_cube(x, y))
            {
                misplaced += before == after ? 0 : 1; // the cube itself is unchanged
                continue;
            }
            int nearest = 1000; // squared distance to the cube
            for (int dy = -3; dy <= 3; ++dy)
            {
                for (int dx = -3; dx <= 3; ++dx)
                {
                    if (in_cube(x + dx, y + dy))
                    {
                        nearest = std::min(nearest, dx * dx + dy * dy);
                    }
                }
            }
            const bool is_outline = same_rgb(after, outline);
            outlined += is_outline ? 1 : 0;
            if (nearest <= 4 && !is_outline)
            {
                ++missed; // within 2 px: outlined
            }
            if (nearest > 4 && !(after == before))
            {
                ++misplaced; // farther: unchanged
            }
        }
    }
    INFO("outlined " << outlined << ", missed " << missed << ", misplaced " << misplaced);
    CHECK(outlined > 50);
    CHECK(missed == 0);
    CHECK(misplaced == 0);
}
