// The debug blit, the pixel inspector and the renderer's debug counters (docs/tasks/11-renderer.md §11.20): the CPU
// helpers as unit tests, then frames on the renderer fixture, with the debug layer and GPU-based validation.

#include "helpers/renderer_fixture.h"
#include "rendering/debug_blit.h"
#include "rendering/view.h"

#include "avis/graphics/graph_debug_information.h"

#include <Eigen/Core>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::renderer_fixture;
using avis::tests::rgba8;
using avis::tests::rgba8_image;

namespace
{
    // A frame without TAA jitter or sky, so the images of two frames can be compared
    view_description steady_view(renderer_fixture& fixture)
    {
        fixture.renderer->set_environment_override(Eigen::Vector3f::Zero());
        view_description made = fixture.view();
        made.options.taa = false;
        return made;
    }

    bool close(const rgba8 pixel, const rgba8 expected, const int tolerance = 3)
    {
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            if (std::abs(int{ pixel[channel] } - int{ expected[channel] }) > tolerance)
            {
                return false;
            }
        }
        return true;
    }

    std::size_t count_close(const rgba8_image& image, const rgba8 expected)
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(image.pixels, [expected](const rgba8 pixel) { return close(pixel, expected); }));
    }

    const graphics::resource_debug_information* find_resource(renderer_fixture& fixture, const std::string_view name)
    {
        const renderer_debug_information information = fixture.renderer->debug_information();
        REQUIRE(information.graph != nullptr);
        const auto found =
            std::ranges::find(information.graph->resources, name, &graphics::resource_debug_information::name);
        return found != information.graph->resources.end() ? &*found : nullptr;
    }

    // One frame inspecting blit `selection` at (x, y); its single result
    inspector_result inspect(renderer_fixture& fixture, view_description view, const inspector_request request)
    {
        view.options.inspect = request;
        fixture.render_and_read({ &view, 1 });
        std::vector<inspector_result> results = fixture.renderer->take_inspector_results();
        REQUIRE(results.size() == 1);
        CHECK(results[0].view == view.name);
        CHECK(results[0].request.x == request.x);
        CHECK(results[0].request.y == request.y);
        return results[0];
    }
} // namespace

// ---- CPU ----

TEST_CASE("a debug blit's rectangle: fullscreen, or flush with a corner", "[renderer][unit]")
{
    const graphics::texture_extent view{ 128, 72 };
    debug_selection selection;
    CHECK(debug_blit_rectangle(selection, view) == view_rectangle{ 0, 0, 128, 72 });

    selection.picture_in_picture = picture_in_picture{ .corner = view_corner::bottom_right, .scale = 0.25f };
    CHECK(debug_blit_rectangle(selection, view) == view_rectangle{ 96, 54, 32, 18 });
    selection.picture_in_picture->corner = view_corner::top_left;
    CHECK(debug_blit_rectangle(selection, view) == view_rectangle{ 0, 0, 32, 18 });
    selection.picture_in_picture->corner = view_corner::top_right;
    CHECK(debug_blit_rectangle(selection, view) == view_rectangle{ 96, 0, 32, 18 });
    selection.picture_in_picture->corner = view_corner::bottom_left;
    CHECK(debug_blit_rectangle(selection, view) == view_rectangle{ 0, 54, 32, 18 });

    selection.picture_in_picture->scale = 0.0f; // at least a pixel
    CHECK(debug_blit_rectangle(selection, view) == view_rectangle{ 0, 71, 1, 1 });
    selection.picture_in_picture->scale = 2.0f; // at most the view
    CHECK(debug_blit_rectangle(selection, view) == view_rectangle{ 0, 0, 128, 72 });
}

TEST_CASE("a selection's name resolves within its view", "[renderer][unit]")
{
    CHECK(resource_in_view("gbuffer_normal", "main") == std::optional<std::string_view>{ "gbuffer_normal" });
    CHECK(resource_in_view("main/gbuffer_normal", "main") == std::optional<std::string_view>{ "gbuffer_normal" });
    CHECK_FALSE(resource_in_view("top/gbuffer_normal", "main").has_value());
}

TEST_CASE("the inspector maps a cursor through the blit's rectangle into the mip", "[renderer][unit]")
{
    const view_rectangle full{ 0, 0, 128, 72 };
    CHECK(inspected_texel(full, { 128, 72 }, 64, 36) == texel_position{ 64, 36 });
    CHECK(inspected_texel(full, { 64, 36 }, 127, 71) == texel_position{ 63, 35 }); // mip 1
    CHECK(inspected_texel(full, { 1, 1 }, 5, 5) == texel_position{ 0, 0 });
    CHECK_FALSE(inspected_texel(full, { 128, 72 }, 128, 0).has_value());

    const view_rectangle corner{ 96, 54, 32, 18 };
    CHECK(inspected_texel(corner, { 128, 72 }, 96, 54) == texel_position{ 2, 2 });
    CHECK(inspected_texel(corner, { 128, 72 }, 127, 71) == texel_position{ 126, 70 });
    CHECK_FALSE(inspected_texel(corner, { 128, 72 }, 95, 60).has_value());
    CHECK_FALSE(inspected_texel(corner, { 128, 72 }, 100, 53).has_value());

    std::vector<std::byte> bytes(gpu::debug_inspector_bytes);
    const float value[4]{ 1.0f, 2.5f, -3.0f, 4.0f };
    const std::uint32_t bits[4]{ 7, 8, 9, 10 };
    std::memcpy(bytes.data(), value, sizeof(value));
    std::memcpy(bytes.data() + 16, bits, sizeof(bits));
    const std::optional<inspected_value> decoded = decode_inspected_value(bytes, { 3, 4 });
    REQUIRE(decoded.has_value());
    CHECK(decoded->texel == texel_position{ 3, 4 });
    CHECK(decoded->value[1] == 2.5f);
    CHECK(decoded->bits[3] == 10u);
    CHECK_FALSE(decode_inspected_value(std::span{ bytes }.first(16), { 0, 0 }).has_value());
}

// ---- GPU ----

TEST_CASE("blitting gbuffer_normal in normal_octahedral mode shows the face normals", "[renderer]")
{
    renderer_fixture fixture;
    // Up and to the right: the camera sees the front (+Z), the left (−X) and the bottom (−Y) faces
    fixture.add_unit_cube({ 1.2f, 0.7f, -4.0f });
    fixture.publish();
    view_description view = steady_view(fixture);
    view.options.debug_blits.push_back(
        { .resource = "main/gbuffer_normal", .mode = debug_blit_mode::normal_octahedral });
    const rgba8_image image = fixture.render_and_read({ &view, 1 }).front();

    // n · 0.5 + 0.5, written as is
    CHECK(count_close(image, { 128, 128, 255, 255 }) > 50); // +Z
    CHECK(count_close(image, { 0, 128, 128, 255 }) > 10); // −X
    CHECK(count_close(image, { 128, 0, 128, 255 }) > 10); // −Y
    CHECK(count_close(image, { 255, 128, 128, 255 }) == 0); // +X faces away
}

TEST_CASE("a picture-in-picture blit leaves the rest of ldr_color unchanged", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_unit_cube({ 0.0f, 0.0f, -4.0f });
    fixture.add_sun({ -1.0f, -1.0f, -1.0f });
    fixture.publish();
    view_description view = steady_view(fixture);
    const rgba8_image plain = fixture.render_and_read({ &view, 1 }).front();

    view.options.debug_blits.push_back(
        { .resource = "linear_depth",
          .mode = debug_blit_mode::depth_linear,
          .range_minimum = 3.0f,
          .range_maximum = 6.0f,
          .picture_in_picture = picture_in_picture{ .corner = view_corner::bottom_right, .scale = 0.5f } });
    const rgba8_image blitted = fixture.render_and_read({ &view, 1 }).front();
    REQUIRE(blitted.pixels.size() == plain.pixels.size());

    const view_rectangle rectangle = debug_blit_rectangle(view.options.debug_blits[0], view.size);
    std::size_t inside_changed = 0;
    std::size_t outside_changed = 0;
    for (std::uint32_t y = 0; y < view.size.height; ++y)
    {
        for (std::uint32_t x = 0; x < view.size.width; ++x)
        {
            const bool inside = x >= rectangle.x && y >= rectangle.y;
            const bool changed = blitted.at(x, y) != plain.at(x, y);
            (inside ? inside_changed : outside_changed) += changed ? 1 : 0;
        }
    }
    CHECK(outside_changed == 0);
    CHECK(inside_changed > rectangle.width * rectangle.height / 2);
    // The cube's centre, 3.5 m away, in the middle of the corner: (3.5 − 3) / 3 of white; nothing drawn is white
    CHECK(
        close(blitted.at(rectangle.x + rectangle.width / 2, rectangle.y + rectangle.height / 2), { 43, 43, 43, 255 }));
    CHECK(blitted.at(rectangle.x + 1, rectangle.y + 1) == rgba8{ 255, 255, 255, 255 });
}

TEST_CASE("a selected transient resource isn't aliased", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_unit_cube({ 0.0f, 0.0f, -4.0f });
    fixture.publish();
    view_description view = steady_view(fixture);

    // A view texture the frame aliases without a selection
    fixture.render_and_read({ &view, 1 });
    std::string aliased;
    for (const std::string_view name : { "gbuffer_albedo",
                                         "gbuffer_material",
                                         "gbuffer_emissive",
                                         "velocity",
                                         "ao_raw",
                                         "ao_edges",
                                         "reactive_mask",
                                         "hdr_color",
                                         "linear_depth" })
    {
        const graphics::resource_debug_information* plain = find_resource(fixture, std::format("main/{}", name));
        REQUIRE(plain != nullptr);
        CHECK_FALSE(plain->debug_selected);
        if (plain->aliased && aliased.empty())
        {
            aliased = name;
        }
    }
    REQUIRE_FALSE(aliased.empty());

    view.options.debug_blits.push_back({ .resource = aliased, .channels = debug_channel_red });
    fixture.render_and_read({ &view, 1 });
    const graphics::resource_debug_information* selected = find_resource(fixture, "main/" + aliased);
    REQUIRE(selected != nullptr);
    CHECK(selected->debug_selected);
    CHECK_FALSE(selected->aliased);
    CHECK(selected->heap_offset != graphics::resource_debug_information::no_offset);
}

TEST_CASE("the pixel inspector returns the linear depth at the centre of the cube", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_unit_cube({ 0.0f, 0.0f, -5.0f });
    fixture.publish();
    view_description view = steady_view(fixture);
    const std::uint32_t centre_x = view.size.width / 2;
    const std::uint32_t centre_y = view.size.height / 2;

    view.options.debug_blits.push_back({ .resource = "linear_depth", .mode = debug_blit_mode::depth_linear });
    view.options.debug_blits.push_back(
        { .resource = "main/depth",
          .mode = debug_blit_mode::depth_linear,
          .picture_in_picture = picture_in_picture{ .corner = view_corner::top_left, .scale = 0.5f } });

    const inspector_result linear = inspect(fixture, view, { .x = centre_x, .y = centre_y, .selection = 0 });
    CHECK(linear.resource == "main/linear_depth");
    CHECK(linear.frame_index > 0);
    REQUIRE(linear.value.has_value());
    CHECK(linear.value->texel == texel_position{ centre_x, centre_y });
    CHECK(linear.value->value[0] == Catch::Approx(4.5f).margin(1e-3));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &linear.value->value[0], sizeof(bits));
    CHECK(linear.value->bits[0] == bits);

    // The depth buffer through the picture-in-picture in the top-left quarter: the centre of its pixel (32, 18) is the
    // corner of texel (65, 37), next to the view's centre
    const inspector_result device =
        inspect(fixture, view, { .x = view.size.width / 4, .y = view.size.height / 4, .selection = 1 });
    CHECK(device.resource == "main/depth");
    REQUIRE(device.value.has_value());
    CHECK(device.value->texel == texel_position{ centre_x + 1, centre_y + 1 });
    CHECK(device.value->value[0] == Catch::Approx(0.1f / 4.5f).epsilon(1e-4)); // reverse-Z: near / distance
    CHECK(device.value->value[1] == Catch::Approx(4.5f).epsilon(1e-3));

    // Outside the picture-in-picture, and without a blit: a result without a value
    CHECK_FALSE(inspect(fixture, view, { .x = centre_x + 10, .y = centre_y, .selection = 1 }).value.has_value());
    CHECK_FALSE(inspect(fixture, view, { .x = centre_x, .y = centre_y, .selection = 5 }).value.has_value());
}

TEST_CASE("an integer resource is inspected as integers, and cluster_lights by the pixel's cluster", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_unit_cube({ 0.0f, 0.0f, -4.0f });
    fixture.add_point_light({ 0.0f, 0.0f, -3.0f }, 100.0f, 5.0f);
    fixture.add_sun({ -1.0f, -1.0f, -1.0f });
    fixture.publish();
    view_description view = steady_view(fixture);
    const std::uint32_t centre_x = view.size.width / 2;
    const std::uint32_t centre_y = view.size.height / 2;
    view.options.debug_blits.push_back({ .resource = "cascade_index", .mode = debug_blit_mode::cascade_index });
    view.options.debug_blits.push_back(
        { .resource = "cluster_lights",
          .mode = debug_blit_mode::cluster_light_count,
          .range_maximum = 4.0f,
          .picture_in_picture = picture_in_picture{ .corner = view_corner::top_left, .scale = 1.0f } });

    const inspector_result cascade = inspect(fixture, view, { .x = centre_x, .y = centre_y, .selection = 0 });
    REQUIRE(cascade.value.has_value());
    CHECK(cascade.value->bits[0] == 0u); // the nearest cascade
    CHECK(cascade.value->value[0] == 0.0f);

    const inspector_result cluster = inspect(fixture, view, { .x = centre_x, .y = centre_y, .selection = 1 });
    REQUIRE(cluster.value.has_value());
    CHECK(cluster.value->value[0] == Catch::Approx(3.5f).margin(1e-3)); // the pixel's linear depth
    CHECK(cluster.value->bits[1] == 1u); // the point light
    CHECK(cluster.value->bits[2] == 0u); // nothing dropped
}

TEST_CASE("every debug blit mode renders, and a selection that can't be shown is skipped", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_unit_cube({ 0.0f, 0.0f, -4.0f });
    fixture.add_sun({ -1.0f, -1.0f, -1.0f });
    fixture.add_point_light({ 0.0f, 0.0f, -3.0f }, 100.0f, 5.0f);
    fixture.publish();
    view_description view = steady_view(fixture);
    const rgba8_image plain = fixture.render_and_read({ &view, 1 }).front();

    const auto corner = [](const view_corner where) { return picture_in_picture{ .corner = where, .scale = 0.25f }; };
    view.options.debug_blits = {
        { .resource = "hdr_color", .mode = debug_blit_mode::luminance_heat },
        { .resource = "velocity",
          .mode = debug_blit_mode::velocity,
          .picture_in_picture = corner(view_corner::top_left) },
        { .resource = "ao", .mode = debug_blit_mode::ao, .picture_in_picture = corner(view_corner::top_right) },
        { .resource = "gbuffer_albedo",
          .mode = debug_blit_mode::alpha_only,
          .picture_in_picture = corner(view_corner::bottom_left) },
        { .resource = "depth_mips",
          .mip = 2,
          .channels = debug_channel_green,
          .range_maximum = 10.0f,
          .picture_in_picture = corner(view_corner::bottom_right) },
        { .resource = "shadow_map", .slice = 1, .picture_in_picture = picture_in_picture{ .scale = 0.1f } },
        // Skipped: another view's, no such resource, a mip out of range, a mismatched mode, the target itself, and a
        // resource without a click
        { .resource = "top/hdr_color" },
        { .resource = "nothing" },
        { .resource = "linear_depth", .mip = 1 },
        { .resource = "hdr_color", .mode = debug_blit_mode::cluster_light_count },
        { .resource = "cluster_lights", .mode = debug_blit_mode::raw },
        { .resource = "ldr_color" },
        { .resource = "picking_id" },
    };
    const rgba8_image blitted = fixture.render_and_read({ &view, 1 }).front();
    CHECK(blitted.pixels != plain.pixels);
}

TEST_CASE("debug_information counts the scene passes' draws, the groups' items and the clusters", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_unit_cube({ 0.0f, 0.0f, -4.0f });
    fixture.add_unit_cube({ 1.5f, 0.0f, -6.0f });
    fixture.add_point_light({ 0.0f, 0.0f, -3.0f }, 100.0f, 5.0f);
    fixture.publish();
    const view_description view = steady_view(fixture);
    fixture.render_and_read({ &view, 1 }, 3);

    const renderer_debug_information information = fixture.renderer->debug_information();
    const auto gbuffer =
        std::ranges::find(information.passes, std::string_view{ "gbuffer" }, &pass_draw_statistics::pass);
    REQUIRE(gbuffer != information.passes.end());
    CHECK(gbuffer->view == "main");
    CHECK(gbuffer->parts >= 1);
    CHECK(gbuffer->parts_recorded == gbuffer->parts);
    CHECK(gbuffer->walk.instances == 2);
    CHECK(gbuffer->walk.draws >= 1);
    CHECK(gbuffer->walk.pipeline_changes >= 1);
    CHECK(
        std::ranges::find(information.passes, std::string_view{ "depth_prepass" }, &pass_draw_statistics::pass) !=
        information.passes.end());

    REQUIRE(information.draw_groups.size() == 1);
    REQUIRE_FALSE(information.draw_groups[0].empty());
    std::size_t items = 0;
    for (const draw_group_items& group : information.draw_groups[0])
    {
        CHECK_FALSE(group.group.empty());
        items += group.items;
    }
    REQUIRE(information.draw_lists.size() == 1);
    CHECK(items == information.draw_lists[0].items);

    REQUIRE(information.clusters.size() == 1);
    CHECK(information.clusters[0].view == "main");
    CHECK(information.clusters[0].frame_index > 0);
    CHECK(information.clusters[0].dropped_lights == 0);
    CHECK(information.clusters[0].overflowing_clusters == 0);
    CHECK(information.clusters[0].most_lights == 1);
    CHECK(information.clusters[0].light_references >= 1);
}
