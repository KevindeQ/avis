// Extraction, interpolation and pass 0 (docs/tasks/11-renderer.md §11.5, 11 §3.5): alpha from the time anchor, slerped
// rotations, the previous rendered world per object, the lights, and the reuse shortcut. The extraction tests run on
// hand-built snapshots without a device; the last ones go through the renderer.

#include "helpers/recorded_assertions.h"
#include "helpers/renderer_fixture.h"
#include "rendering/extraction.h"
#include "rendering/scene_renderer.h"

#include "avis/graphics/graph_debug_information.h"
#include "avis/scene/flags.h"
#include "avis/scene/snapshot.h"
#include "avis/scene/types.h"

#include <Eigen/Geometry>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <optional>
#include <string_view>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;
using Catch::Approx;
using namespace std::chrono_literals;

namespace
{
    const auto wall_base = std::chrono::steady_clock::time_point{} + 1000s;

    // Ticks at 100 ms and 200 ms, running at rate 1 from the first: alpha is (wall_now − wall_base) / 100 ms
    scene::snapshot running_snapshot()
    {
        scene::snapshot snapshot;
        snapshot.time_previous = 100ms;
        snapshot.time_current = 200ms;
        snapshot.anchor = { .t_base = 100ms, .wall_base = wall_base, .rate = 1.0 };
        snapshot.publish_sequence = 1;
        return snapshot;
    }

    constexpr scene::mesh_handle mesh(const std::uint32_t index) noexcept
    {
        return { .index = index, .generation = 1 };
    }

    constexpr scene::material_handle material(const std::uint32_t index) noexcept
    {
        return { .index = index, .generation = 1 };
    }

    Eigen::Matrix4f translation(const float x, const float y = 0.0f, const float z = 0.0f)
    {
        Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
        matrix.col(3).head<3>() = Eigen::Vector3f{ x, y, z };
        return matrix;
    }

    scene::render_instance
        moving_instance(const scene::object_id id, const Eigen::Matrix4f& previous, const Eigen::Matrix4f& current)
    {
        scene::render_instance instance;
        instance.object_id = id;
        instance.mesh = mesh(1);
        instance.material = material(1);
        instance.world_previous = previous;
        instance.world_current = current;
        return instance;
    }

    // Every mesh is resident at version 7 except mesh 9; material slots are the handle index + 100
    extraction_lookup lookup()
    {
        return {
            .mesh_version = [](const scene::mesh_handle handle) -> std::optional<std::uint64_t>
            { return handle.index == 9 ? std::nullopt : std::optional<std::uint64_t>{ 7 }; },
            .material_slot = [](const scene::material_handle handle) { return handle.index + 100; },
        };
    }

    Eigen::Matrix4f to_eigen(const gpu::float4x4& matrix)
    {
        Eigen::Matrix4f converted;
        std::memcpy(converted.data(), &matrix, sizeof(matrix));
        return converted;
    }

    float translation_x(const gpu::float4x4& matrix)
    {
        return matrix.columns[3].x;
    }
} // namespace

TEST_CASE("alpha follows the time anchor and is clamped to the tick", "[renderer][unit]")
{
    scene::snapshot snapshot = running_snapshot();

    // Halfway between the ticks
    frame_interpolation halfway = interpolate(snapshot, wall_base + 50ms);
    CHECK(halfway.alpha == Approx(0.5f));
    CHECK(halfway.t_render == 150ms);

    // Past the end of the tick: holds at t_k instead of extrapolating
    frame_interpolation late = interpolate(snapshot, wall_base + 500ms);
    CHECK(late.alpha == 1.0f);
    CHECK(late.t_render == 200ms);

    // Before the anchor: clamped to t_{k−1}
    frame_interpolation early = interpolate(snapshot, wall_base - 10ms);
    CHECK(early.alpha == 0.0f);
    CHECK(early.t_render == 100ms);

    // Half speed: a quarter of the tick after 50 ms
    snapshot.anchor.rate = 0.5;
    CHECK(interpolate(snapshot, wall_base + 50ms).alpha == Approx(0.25f));

    // Paused: alpha 1 whatever the wall time
    snapshot.anchor.rate = 0.0;
    snapshot.anchor.t_base = 100ms;
    frame_interpolation paused = interpolate(snapshot, wall_base + 50ms);
    CHECK(paused.alpha == 1.0f);
    CHECK(paused.t_render == 200ms);

    // Two ticks at the same time (the first snapshot): alpha 1
    scene::snapshot first;
    first.anchor.rate = 1.0;
    CHECK(interpolate(first, wall_base).alpha == 1.0f);
}

TEST_CASE("a 90 degree rotation interpolates through 45 degrees", "[renderer][unit]")
{
    Eigen::Matrix4f current = Eigen::Matrix4f::Identity();
    current.topLeftCorner<3, 3>() =
        Eigen::AngleAxisf{ std::numbers::pi_v<float> / 2.0f, Eigen::Vector3f::UnitZ() }.toRotationMatrix();
    current.col(3).head<3>() = Eigen::Vector3f{ 2.0f, 0.0f, 0.0f };

    const Eigen::Matrix4f halfway = interpolate_world(Eigen::Matrix4f::Identity(), current, 0.5f);

    // Slerp: the X axis turned by 45° keeps unit length (a matrix lerp would give (0.5, 0.5), of length 0.71)
    const float diagonal = std::sqrt(0.5f);
    CHECK(halfway(0, 0) == Approx(diagonal).margin(1e-5));
    CHECK(halfway(1, 0) == Approx(diagonal).margin(1e-5));
    CHECK(halfway.col(0).head<3>().norm() == Approx(1.0f).margin(1e-5));
    CHECK(halfway(0, 3) == Approx(1.0f).margin(1e-5));

    // The ends are the matrices themselves, exactly
    CHECK(interpolate_world(Eigen::Matrix4f::Identity(), current, 1.0f) == current);
    CHECK(interpolate_world(Eigen::Matrix4f::Identity(), current, 0.0f) == Eigen::Matrix4f::Identity());

    // Scale interpolates linearly, with the rotation
    Eigen::Matrix4f scaled = current;
    scaled.topLeftCorner<3, 3>() *= 3.0f;
    const Eigen::Matrix4f scaled_halfway = interpolate_world(Eigen::Matrix4f::Identity(), scaled, 0.5f);
    CHECK(scaled_halfway.col(0).head<3>().norm() == Approx(2.0f).margin(1e-4));
    CHECK(scaled_halfway(1, 0) / scaled_halfway(0, 0) == Approx(1.0f).margin(1e-4));
}

TEST_CASE("instances carry the interpolated world, mesh, material, flags and id", "[renderer][unit]")
{
    scene::snapshot snapshot = running_snapshot();
    scene::render_instance first = moving_instance(0x1234'5678'9ABC'DEF0ull, translation(0.0f), translation(4.0f));
    first.flags = scene::instance_flags::visible | scene::instance_flags::selected;
    first.layer_priority = 3;

    scene::render_instance second = moving_instance(2, translation(1.0f), translation(1.0f));
    second.mesh = mesh(2);
    second.material = {}; // the lookup's slot for an invalid handle

    scene::render_instance same_mesh = moving_instance(3, translation(5.0f), translation(5.0f));
    scene::render_instance not_resident = moving_instance(4, translation(0.0f), translation(0.0f));
    not_resident.mesh = mesh(9);
    scene::render_instance invisible = moving_instance(5, translation(0.0f), translation(0.0f));
    invisible.flags = scene::instance_flags::casts_shadows;

    snapshot.instances = { first, second, not_resident, invisible, same_mesh };

    extraction_state state;
    const extraction_result result = extract(snapshot, wall_base + 25ms, state, lookup());
    CHECK(result.interpolation.alpha == Approx(0.25f));
    CHECK(result.publish_sequence == 1);
    REQUIRE(result.instances.size() == 3);
    CHECK(result.instance_sources == std::vector<std::uint32_t>{ 0, 1, 4 });
    CHECK(result.skipped_not_resident == 1);
    CHECK(result.skipped_invisible == 1);

    // Two meshes, each once, in order of first use
    REQUIRE(result.meshes.size() == 2);
    CHECK(result.meshes[0] == frame_mesh{ .handle = mesh(1), .version = 7 });
    CHECK(result.meshes[1] == frame_mesh{ .handle = mesh(2), .version = 7 });

    const gpu::instance_data& data = result.instances[0];
    CHECK(translation_x(data.world) == Approx(1.0f));
    CHECK(data.mesh == 0);
    CHECK(data.material == 101);
    CHECK(data.flags == (gpu::instance_flag_visible | gpu::instance_flag_selected));
    CHECK(data.layer_priority == 3);
    CHECK(data.object_id_low == 0x9ABC'DEF0u);
    CHECK(data.object_id_high == 0x1234'5678u);
    CHECK(to_eigen(data.normal_from_object) == Eigen::Matrix4f::Identity());

    CHECK(result.instances[1].mesh == 1);
    CHECK(result.instances[1].material == 100);
    CHECK(result.instances[2].mesh == 0);
}

TEST_CASE("the normal matrix is the inverse transpose of the world's linear part", "[renderer][unit]")
{
    scene::snapshot snapshot;
    Eigen::Matrix4f world = Eigen::Matrix4f::Identity();
    world.topLeftCorner<3, 3>() = Eigen::Vector3f{ 2.0f, 4.0f, 0.5f }.asDiagonal();
    snapshot.instances = { moving_instance(1, world, world) };

    extraction_state state;
    const extraction_result result = extract(snapshot, wall_base, state, lookup());
    REQUIRE(result.instances.size() == 1);
    const Eigen::Matrix4f normal = to_eigen(result.instances[0].normal_from_object);
    CHECK(normal(0, 0) == Approx(0.5f));
    CHECK(normal(1, 1) == Approx(0.25f));
    CHECK(normal(2, 2) == Approx(2.0f));
    CHECK(normal(3, 3) == 1.0f);
}

TEST_CASE("the previous world is the previous rendered frame's, not the previous tick's", "[renderer][unit]")
{
    scene::snapshot snapshot = running_snapshot();
    snapshot.instances = { moving_instance(1, translation(0.0f), translation(4.0f)) };

    extraction_state state;

    // A new id: no motion
    extraction_result first = extract(snapshot, wall_base + 25ms, state, lookup());
    REQUIRE(first.instances.size() == 1);
    CHECK(translation_x(first.instances[0].world) == Approx(1.0f));
    CHECK(translation_x(first.instances[0].world_previous) == Approx(1.0f));
    CHECK(first.stationary);
    REQUIRE(state.previous_world(1) != nullptr);

    // The second frame's previous world is the first frame's interpolated world (x = 1), not world_previous (x = 0)
    extraction_result second = extract(snapshot, wall_base + 75ms, state, lookup());
    REQUIRE(second.instances.size() == 1);
    CHECK(translation_x(second.instances[0].world) == Approx(3.0f));
    CHECK(translation_x(second.instances[0].world_previous) == Approx(1.0f));
    CHECK_FALSE(second.stationary);

    // A jump (world_previous == world_current) restarts without motion, wherever the last frame drew it
    snapshot.instances = { moving_instance(1, translation(10.0f), translation(10.0f)) };
    extraction_result jumped = extract(snapshot, wall_base + 75ms, state, lookup());
    REQUIRE(jumped.instances.size() == 1);
    CHECK(translation_x(jumped.instances[0].world) == Approx(10.0f));
    CHECK(translation_x(jumped.instances[0].world_previous) == Approx(10.0f));

    // history_invalid: a pop-in at world_current, without motion
    scene::render_instance popped = moving_instance(1, translation(20.0f), translation(20.0f));
    popped.flags = scene::default_instance_flags | scene::instance_flags::history_invalid;
    snapshot.instances = { popped };
    extraction_result popped_in = extract(snapshot, wall_base + 25ms, state, lookup());
    REQUIRE(popped_in.instances.size() == 1);
    CHECK(translation_x(popped_in.instances[0].world) == Approx(20.0f));
    CHECK(translation_x(popped_in.instances[0].world_previous) == Approx(20.0f));
    CHECK((popped_in.instances[0].flags & gpu::instance_flag_history_invalid) != 0);

    // A new id next to a known one: the new one has no motion
    snapshot.instances = { moving_instance(1, translation(20.0f), translation(24.0f)),
                           moving_instance(2, translation(0.0f), translation(4.0f)) };
    extraction_result mixed = extract(snapshot, wall_base + 50ms, state, lookup());
    REQUIRE(mixed.instances.size() == 2);
    CHECK(translation_x(mixed.instances[0].world_previous) == Approx(20.0f));
    CHECK(translation_x(mixed.instances[0].world) == Approx(22.0f));
    CHECK(translation_x(mixed.instances[1].world_previous) == Approx(2.0f));
    CHECK(state.size() == 2);

    // Ids not drawn this frame are dropped: when it comes back, it has no motion
    snapshot.instances = { moving_instance(2, translation(0.0f), translation(4.0f)) };
    extract(snapshot, wall_base + 50ms, state, lookup());
    CHECK(state.size() == 1);
    CHECK(state.previous_world(1) == nullptr);
}

TEST_CASE("lights: the first directional light is the sun, past the limit they are dropped", "[renderer][unit]")
{
    scene::snapshot snapshot;

    scene::light_instance point;
    point.kind = scene::light_kind::point;
    point.intensity = 800.0f;
    point.range = 12.0f;
    point.light_color = { 1.0f, 0.5f, 0.25f, 1.0f };
    point.world = translation(1.0f, 2.0f, 3.0f);
    point.casts_shadows = true; // only the sun casts shadows in iteration 1

    scene::light_instance sun;
    sun.kind = scene::light_kind::directional;
    sun.intensity = 100'000.0f;
    sun.casts_shadows = true;
    // −Z turned to point down: the rotation by −90° about X takes −Z to −Y
    sun.world.topLeftCorner<3, 3>() =
        Eigen::AngleAxisf{ -std::numbers::pi_v<float> / 2.0f, Eigen::Vector3f::UnitX() }.toRotationMatrix();

    scene::light_instance second_sun = sun;

    scene::light_instance spot;
    spot.kind = scene::light_kind::spot;
    spot.inner_cone_angle = 0.0f;
    spot.outer_cone_angle = std::numbers::pi_v<float> / 3.0f;

    snapshot.lights = { point, sun, second_sun, spot };

    extraction_state state;
    const extraction_result result = extract(snapshot, wall_base, state, lookup());
    REQUIRE(result.lights.size() == 4);
    REQUIRE(result.sun.has_value());
    CHECK(*result.sun == 1);

    const gpu::light_data& first = result.lights[0];
    CHECK(first.kind == gpu::light_kind_point);
    CHECK(first.position.x == 1.0f);
    CHECK(first.position.y == 2.0f);
    CHECK(first.position.z == 3.0f);
    CHECK(first.range == 12.0f);
    CHECK(first.intensity == 800.0f);
    CHECK(first.color.y == 0.5f);
    CHECK(first.casts_shadows == 0);

    const gpu::light_data& sun_data = result.lights[1];
    CHECK(sun_data.kind == gpu::light_kind_directional);
    CHECK(sun_data.direction.x == Approx(0.0f).margin(1e-6));
    CHECK(sun_data.direction.y == Approx(-1.0f));
    CHECK(sun_data.direction.z == Approx(0.0f).margin(1e-6));
    CHECK(sun_data.casts_shadows == 1);
    CHECK(result.lights[2].casts_shadows == 0); // a second directional light isn't the sun

    CHECK(result.lights[3].kind == gpu::light_kind_spot);
    CHECK(result.lights[3].spot_cos_inner == Approx(1.0f));
    CHECK(result.lights[3].spot_cos_outer == Approx(0.5f));

    snapshot.lights.assign(maximum_lights + 3, point);
    const extraction_result truncated = extract(snapshot, wall_base, state, lookup());
    CHECK(truncated.lights.size() == maximum_lights);
    CHECK(truncated.dropped_lights == 3);
    CHECK_FALSE(truncated.sun.has_value());
}

TEST_CASE("a screen-aligned instance in the world layer is a debug check", "[renderer][unit]")
{
    tests::recorded_assertions assertions;
    scene::snapshot snapshot;
    scene::render_instance label = moving_instance(1, translation(0.0f), translation(0.0f));
    label.flags = scene::default_instance_flags | scene::instance_flags::screen_aligned;
    scene::render_instance overlay_label = label;
    overlay_label.object_id = 2;
    overlay_label.layer = scene::draw_layer::overlay;
    snapshot.instances = { label, overlay_label };

    extraction_state state;
    const extraction_result result = extract(snapshot, wall_base, state, lookup());
    CHECK(result.skipped_invalid == 1);
    REQUIRE(result.instances.size() == 1);
    CHECK(result.instance_sources[0] == 1);
#ifndef NDEBUG
    CHECK(assertions.count() == 1);
    CHECK(assertions.contains("screen_aligned"));
#endif
}

TEST_CASE("extraction keeps the result's capacity across frames", "[renderer][unit]")
{
    scene::snapshot snapshot;
    snapshot.instances.assign(64, moving_instance(1, translation(0.0f), translation(0.0f)));
    for (std::size_t index = 0; index < snapshot.instances.size(); ++index)
    {
        snapshot.instances[index].object_id = index + 1;
    }

    extraction_state state;
    extraction_result result;
    extract(snapshot, wall_base, state, lookup(), result);
    const gpu::instance_data* storage = result.instances.data();
    extract(snapshot, wall_base, state, lookup(), result);
    CHECK(result.instances.size() == 64);
    CHECK(result.instances.data() == storage);
}

namespace
{
    const graphics::pass_debug_information* find_pass(const tests::renderer_fixture& fixture, std::string_view name)
    {
        const renderer_debug_information information = fixture.renderer->debug_information();
        REQUIRE(information.graph != nullptr);
        const auto found = std::ranges::find(information.graph->passes, name, &graphics::pass_debug_information::name);
        return found == information.graph->passes.end() ? nullptr : &*found;
    }
} // namespace

TEST_CASE_METHOD(tests::renderer_fixture, "the renderer extracts the snapshot and uploads it in pass 0", "[renderer]")
{
    add_unit_cube({ 0.0f, 0.0f, -5.0f });
    add_unit_cube({ 2.0f, 0.0f, -5.0f });
    add_sun({ 0.0f, -1.0f, 0.0f });
    add_point_light({ 0.0f, 3.0f, 0.0f }, 100.0f, 10.0f);
    publish();

    const std::vector views{ view() };
    render_and_read(views);

    // Both cubes are resident by the time extraction runs: the flush comes first
    const renderer_debug_information information = renderer->debug_information();
    CHECK(information.extraction.instances == 2);
    CHECK(information.extraction.meshes == 2);
    CHECK(information.extraction.lights == 2);
    CHECK(information.extraction.skipped_not_resident == 0);
    CHECK(information.frame_data.uploads == 1);
    // Plus the instance indices of the draw runs (11.9): one per drawn item
    CHECK(information.instance_indices == 2);
    CHECK(
        information.frame_data.uploaded_bytes ==
        2 * sizeof(gpu::instance_data) + 2 * sizeof(gpu::light_data) + 2 * sizeof(std::uint32_t));
    REQUIRE(renderer->extraction().sun.has_value());

    const graphics::pass_debug_information* upload = find_pass(*this, "upload_frame_data");
    REQUIRE(upload != nullptr);
    CHECK_FALSE(upload->culled);
    CHECK(renderer->frame_data().instance_count == 2);
    CHECK(renderer->frame_data().light_count == 2);
}

TEST_CASE_METHOD(
    tests::renderer_fixture, "the reuse shortcut skips extraction for the same snapshot and alpha", "[renderer]")
{
    add_unit_cube({ 0.0f, 0.0f, -5.0f });
    publish(); // paused: alpha 1
    const std::vector views{ view() };

    // Two frames on the same snapshot and alpha: the second writes no instance slot
    render_and_read(views);
    renderer_debug_information information = renderer->debug_information();
    CHECK(information.frame_data.uploads == 1);
    CHECK_FALSE(information.extraction.reused);
    const std::uint32_t slot = information.frame_data.instance_slot;

    render_and_read(views, 3);
    information = renderer->debug_information();
    CHECK(information.frame_data.uploads == 1);
    CHECK(information.frame_data.reuses == 3);
    CHECK(information.extraction.reused);
    CHECK(information.frame_data.instance_slot == slot);
    CHECK(information.extraction.instances == 1);
    // Only the instance indices are copied: the draw runs follow the camera (11.9)
    const graphics::pass_debug_information* upload = find_pass(*this, "upload_frame_data");
    REQUIRE(upload != nullptr);
    CHECK_FALSE(upload->culled);
    CHECK(information.instance_indices == 1);

    // A new publish of the same scene: a new sequence, so a new slot
    publish();
    render_and_read(views);
    information = renderer->debug_information();
    CHECK(information.frame_data.uploads == 2);
    CHECK_FALSE(information.extraction.reused);
    CHECK(information.frame_data.instance_slot != slot); // the free slot used longest ago
}

TEST_CASE_METHOD(tests::renderer_fixture, "a changed alpha, or arrays with motion, are extracted again", "[renderer]")
{
    const scene::entity cube = add_unit_cube({ 0.0f, 0.0f, -5.0f });
    publish();
    world.set_local(cube, scene::transform{ .position = { 4.0f, 0.0f, -5.0f } });

    // Running at rate 1 from the first tick: alpha is (wall_now − base) / 100 ms
    const auto base = std::chrono::steady_clock::now();
    time_previous = 100ms;
    time_current = 200ms;
    anchor = { .t_base = 100ms, .wall_base = base, .rate = 1.0 };
    publish();
    const std::vector views{ view() };

    wall_now = base + 25ms; // new to the renderer: no motion
    render_and_read(views);
    CHECK(renderer->debug_information().frame_data.uploads == 1);
    CHECK(renderer->extraction().stationary);

    wall_now = base + 50ms; // a changed alpha: extracted, with motion
    render_and_read(views);
    renderer_debug_information information = renderer->debug_information();
    CHECK(information.frame_data.uploads == 2);
    CHECK(information.extraction.alpha == Approx(0.5f));
    CHECK_FALSE(renderer->extraction().stationary);
    REQUIRE(renderer->extraction().instances.size() == 1);
    CHECK(translation_x(renderer->extraction().instances[0].world) == Approx(2.0f));
    CHECK(translation_x(renderer->extraction().instances[0].world_previous) == Approx(1.0f));

    // The same alpha again: the arrays of the last frame have motion, so they are extracted once more (now without),
    // and only then reused
    render_and_read(views);
    CHECK(renderer->debug_information().frame_data.uploads == 3);
    CHECK(renderer->extraction().stationary);
    render_and_read(views);
    information = renderer->debug_information();
    CHECK(information.frame_data.uploads == 3);
    CHECK(information.frame_data.reuses == 1);
}
