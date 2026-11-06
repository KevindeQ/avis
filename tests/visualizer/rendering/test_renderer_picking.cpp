// Picking (docs/tasks/11-renderer.md §11.19): the id of the pickable instance on top under a click, through the
// picking passes, the pick window and the readback ring; select_pick as a unit test. On the renderer fixture, with the
// debug layer and GPU-based validation.

#include "helpers/renderer_fixture.h"
#include "rendering/picking.h"
#include "rendering/view.h"

#include "avis/rhi/formats.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"

#include <Eigen/Core>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::renderer_fixture;
using avis::tests::renderer_fixture_options;
using avis::tests::rgba8_image;

namespace
{
    // A frame without TAA jitter, so the images of two frames can be compared
    view_description steady_view(renderer_fixture& fixture)
    {
        fixture.renderer->set_environment_override(Eigen::Vector3f::Zero());
        view_description made = fixture.view();
        made.options.taa = false;
        return made;
    }

    scene::object_id id_of(renderer_fixture& fixture, const scene::entity entity)
    {
        return fixture.world.registry().get<scene::components::identity>(entity).id;
    }

    void set_render_options(
        renderer_fixture& fixture,
        const scene::entity entity,
        const scene::draw_layer layer,
        const avis::flags<scene::instance_flags> flags = scene::default_instance_flags)
    {
        fixture.world.registry().emplace_or_replace<scene::components::render_options>(
            entity, scene::components::render_options{ .layer = layer, .flags = flags });
    }

    scene::material_description translucent(const float alpha)
    {
        scene::material_description material;
        material.base_color_factor = { 0.2f, 0.4f, 0.8f, alpha };
        material.alpha = scene::alpha_mode::blend;
        return material;
    }

    scene::material_description unlit_line(const float width)
    {
        scene::material_description material;
        material.base_color_factor = { 1.0f, 1.0f, 0.0f, 1.0f };
        material.shading = scene::shading_model::unlit;
        material.line_width = width;
        return material;
    }

    // A horizontal line through `centre`, `length` long
    scene::mesh_data line_mesh(const float length)
    {
        scene::mesh_builder builder{ scene::mesh_topology::lines };
        builder.add_vertex({ -0.5f * length, 0.0f, 0.0f });
        builder.add_vertex({ 0.5f * length, 0.0f, 0.0f });
        result<scene::mesh_data> mesh = std::move(builder).build();
        REQUIRE(mesh.has_value());
        return std::move(*mesh);
    }

    // Renders one frame of `view` with a click at (x, y) and returns its result
    pick_result pick(renderer_fixture& fixture, view_description view, const std::uint32_t x, const std::uint32_t y)
    {
        view.options.pick = pick_request{ .x = x, .y = y };
        fixture.render_and_read({ &view, 1 });
        std::vector<pick_result> results = fixture.renderer->take_pick_results();
        REQUIRE(results.size() == 1);
        CHECK(results[0].view == view.name);
        CHECK(results[0].request.x == x);
        CHECK(results[0].request.y == y);
        return std::move(results[0]);
    }

    std::optional<scene::object_id> picked(const pick_result& result)
    {
        return result.hit ? std::optional{ result.hit->object_id } : std::nullopt;
    }

    // A window of R32G32_UINT texels with `id` at (column, row)
    std::vector<std::byte> window_with(
        std::vector<std::byte> window, const std::uint32_t column, const std::uint32_t row, const scene::object_id id)
    {
        const std::uint32_t words[2]{ static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(id >> 32) };
        std::memcpy(window.data() + (row * gpu::pick_window_size + column) * 8, words, sizeof(words));
        return window;
    }
} // namespace

TEST_CASE("select_pick takes the non-zero id closest to the centre", "[renderer][unit]")
{
    const std::vector<std::byte> empty(pick_window_bytes);
    const pick_window_origin origin{ .x = 10, .y = 20 };
    CHECK_FALSE(select_pick(empty, origin).has_value());
    CHECK_FALSE(select_pick(std::span{ empty }.first(8), origin).has_value());

    std::vector<std::byte> window = window_with(empty, 0, 0, 7);
    window = window_with(window, 8, 8, 8);
    window = window_with(window, 4, 7, 0x1'0000'0002ull); // 3 below the centre, both words
    window = window_with(window, 1, 4, 9); // 3 left of it: the same distance, later in row order
    CHECK(select_pick(window, origin) == pick_hit{ .object_id = 9, .x = 11, .y = 24 });
    window = window_with(window, 5, 5, 3);
    CHECK(select_pick(window, origin) == pick_hit{ .object_id = 3, .x = 15, .y = 25 });
    window = window_with(window, 4, 4, 4);
    CHECK(select_pick(window, origin) == pick_hit{ .object_id = 4, .x = 14, .y = 24 });

    const pick_window_origin corner = pick_origin({ .x = 0, .y = 0 });
    CHECK(corner.x == -4);
    CHECK(corner.y == -4);
}

TEST_CASE("a click on a cube returns its id, on the background none", "[renderer]")
{
    renderer_fixture fixture;
    const scene::entity cube = fixture.add_unit_cube({ 0.0f, 0.0f, -4.0f });
    fixture.publish();
    const view_description view = steady_view(fixture);
    const std::uint32_t centre_x = view.size.width / 2;
    const std::uint32_t centre_y = view.size.height / 2;

    const pick_result on_cube = pick(fixture, view, centre_x, centre_y);
    REQUIRE(on_cube.hit.has_value());
    CHECK(on_cube.hit->object_id == id_of(fixture, cube));
    CHECK(on_cube.hit->x == centre_x);
    CHECK(on_cube.hit->y == centre_y);
    CHECK(on_cube.frame_index > 0);

    CHECK_FALSE(pick(fixture, view, 2, 2).hit.has_value());
    // At the corner the window reaches out of the view, which reads nothing
    CHECK_FALSE(pick(fixture, view, 0, 0).hit.has_value());
    CHECK_FALSE(pick(fixture, view, view.size.width - 1, view.size.height - 1).hit.has_value());

    // A frame without a click reports nothing
    fixture.render_and_read({ &view, 1 });
    CHECK(fixture.renderer->take_pick_results().empty());
}

TEST_CASE("a translucent box in front of the cube wins, and an overlay line in front of both", "[renderer]")
{
    renderer_fixture fixture;
    const scene::entity cube = fixture.add_unit_cube({ 0.0f, 0.0f, -5.0f });
    const scene::entity box = fixture.add_unit_cube({ 0.0f, 0.0f, -3.0f }, translucent(0.5f));
    fixture.publish();
    const view_description view = steady_view(fixture);
    const std::uint32_t centre_x = view.size.width / 2;
    const std::uint32_t centre_y = view.size.height / 2;
    CHECK(picked(pick(fixture, view, centre_x, centre_y)) == id_of(fixture, box));

    // A nearly transparent box in front of that one lets it through (coverage below 0.1)
    fixture.add_unit_cube({ 0.0f, 0.0f, -2.0f }, translucent(0.05f));
    fixture.publish();
    CHECK(picked(pick(fixture, view, centre_x, centre_y)) == id_of(fixture, box));

    // An overlay line through the centre, behind the cube: overlays are drawn after the scene, but depth-tested
    const scene::entity hidden = fixture.add_mesh("hidden", { 0.0f, 0.0f, -8.0f }, line_mesh(4.0f), unlit_line(3.0f));
    set_render_options(fixture, hidden, scene::draw_layer::overlay);
    fixture.publish();
    CHECK(picked(pick(fixture, view, centre_x, centre_y)) == id_of(fixture, box));

    const scene::entity line = fixture.add_mesh("line", { 0.0f, 0.0f, -1.5f }, line_mesh(4.0f), unlit_line(3.0f));
    set_render_options(fixture, line, scene::draw_layer::overlay);
    fixture.publish();
    CHECK(picked(pick(fixture, view, centre_x, centre_y)) == id_of(fixture, line));

    // On top, the hidden one wins too, being drawn last
    set_render_options(fixture, hidden, scene::draw_layer::overlay_on_top);
    fixture.publish();
    CHECK(picked(pick(fixture, view, centre_x, centre_y)) == id_of(fixture, hidden));
}

TEST_CASE("a 1 px line is picked by a click 3 px away", "[renderer]")
{
    renderer_fixture fixture;
    const scene::entity line = fixture.add_mesh("line", { 0.0f, 0.0f, -3.0f }, line_mesh(3.0f), unlit_line(1.0f));
    set_render_options(fixture, line, scene::draw_layer::overlay);
    fixture.publish();
    const view_description view = steady_view(fixture);
    const std::uint32_t centre_x = view.size.width / 2;
    const std::uint32_t centre_y = view.size.height / 2;

    const pick_result above = pick(fixture, view, centre_x, centre_y - 3);
    REQUIRE(above.hit.has_value());
    CHECK(above.hit->object_id == id_of(fixture, line));
    CHECK(above.hit->x == centre_x);
    // The line's row (or the one above it, where the line's centre falls between two rows)
    CHECK(above.hit->y >= centre_y - 1);
    CHECK(above.hit->y <= centre_y);

    const pick_result below = pick(fixture, view, centre_x + 10, centre_y + 3);
    CHECK(picked(below) == id_of(fixture, line));
    // Beyond the window's reach, nothing
    CHECK_FALSE(pick(fixture, view, centre_x, centre_y - 8).hit.has_value());
}

TEST_CASE("a non-pickable instance in front lets the one behind through", "[renderer]")
{
    renderer_fixture fixture;
    const scene::entity cube = fixture.add_unit_cube({ 0.0f, 0.0f, -5.0f });
    const scene::entity box = fixture.add_unit_cube({ 0.0f, 0.0f, -3.0f }, translucent(0.8f));
    const scene::entity line = fixture.add_mesh("line", { 0.0f, 0.0f, -2.0f }, line_mesh(4.0f), unlit_line(3.0f));
    const avis::flags<scene::instance_flags> not_pickable =
        scene::instance_flags::visible | scene::instance_flags::casts_shadows;
    set_render_options(fixture, box, scene::draw_layer::world, not_pickable);
    set_render_options(fixture, line, scene::draw_layer::overlay, not_pickable);
    fixture.publish();
    const view_description view = steady_view(fixture);
    CHECK(picked(pick(fixture, view, view.size.width / 2, view.size.height / 2)) == id_of(fixture, cube));

    // Not pickable either, the cube leaves nothing
    set_render_options(fixture, cube, scene::draw_layer::world, not_pickable);
    fixture.publish();
    CHECK_FALSE(pick(fixture, view, view.size.width / 2, view.size.height / 2).hit.has_value());
}

TEST_CASE("a click frame and a normal frame give the same image", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_unit_cube({ 0.0f, 0.0f, -5.0f });
    fixture.add_unit_cube({ 0.5f, 0.0f, -3.0f }, translucent(0.5f));
    const scene::entity line = fixture.add_mesh("line", { 0.0f, 0.0f, -2.0f }, line_mesh(4.0f), unlit_line(3.0f));
    set_render_options(fixture, line, scene::draw_layer::overlay);
    fixture.add_sun({ -1.0f, -1.0f, -1.0f });
    fixture.publish();
    view_description view = steady_view(fixture);

    const rgba8_image normal = fixture.render_and_read({ &view, 1 }).front();
    view.options.pick = pick_request{ .x = view.size.width / 2, .y = view.size.height / 2 };
    const rgba8_image clicked = fixture.render_and_read({ &view, 1 }).front();
    CHECK(fixture.renderer->take_pick_results().size() == 1);
    REQUIRE(clicked.pixels.size() == normal.pixels.size());
    CHECK(clicked.pixels == normal.pixels);
}

TEST_CASE("without a click, a frame has no picking target", "[renderer]")
{
    renderer_fixture fixture;
    fixture.add_unit_cube({ 0.0f, 0.0f, -4.0f });
    fixture.publish();
    view_description view = steady_view(fixture);
    view.options.captures.push_back(
        { .resource = "picking_id", .texture = fixture.make_texture(rhi::format::rg32_uint, view.size) });
    CHECK_FALSE(fixture.render_frame({ &view, 1 }).has_value());

    // With one, it is captured: the cube's id at the centre, 0 at the corner
    view.options.pick = pick_request{ .x = 1, .y = 1 };
    fixture.render_and_read({ &view, 1 });
    const graphics::readback_image image = fixture.read_subresource(view.options.captures[0].texture);
    const auto texel = [&](const std::uint32_t x, const std::uint32_t y)
    {
        std::uint32_t words[2];
        std::memcpy(words, image.bytes().data() + std::size_t{ y } * image.row_pitch() + x * 8, sizeof(words));
        return (scene::object_id{ words[1] } << 32) | words[0];
    };
    CHECK(texel(view.size.width / 2, view.size.height / 2) != 0);
    CHECK(texel(0, 0) == 0);
    CHECK(fixture.renderer->take_pick_results().size() == 1);
}
