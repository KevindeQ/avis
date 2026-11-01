// scene's viewing aids: labels, trails, world bounds and reflect (docs/tasks/10-scene-management.md §10.12)

#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"

#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/components/spatial.h"
#include "avis/scene/components/viewing.h"
#include "avis/scene/reflection.h"
#include "avis/scene/scene.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <initializer_list>
#include <numbers>
#include <string>
#include <vector>

using namespace avis;
using namespace avis::scene::components;
using namespace std::chrono_literals;
using avis::scene::entity;
using avis::scene::entity_category;
using avis::scene::transform;

namespace
{
    entity create(
        scene::scene& world,
        const scene::object_id id,
        const std::string& name,
        const entity_category category = entity_category::object,
        const entity parent = entt::null,
        const transform& local = {})
    {
        const result<entity> created =
            world.create_entity({ .id = id, .name = name, .category = category, .parent = parent, .local = local });
        REQUIRE(created);
        return *created;
    }

    transform pose(const Eigen::Vector3f& position, const float yaw = 0.0f)
    {
        transform made;
        made.position = position;
        made.rotation = Eigen::Quaternionf{ Eigen::AngleAxisf{ yaw, Eigen::Vector3f::UnitZ() } };
        return made;
    }

    // A box from `minimum` to `maximum` as one primitive of 8 points (topology doesn't matter for bounds)
    scene::mesh_handle
        make_box(scene::asset_cache& assets, const Eigen::Vector3f& minimum, const Eigen::Vector3f& maximum)
    {
        scene::mesh_builder builder{ scene::mesh_topology::points };
        for (int corner = 0; corner < 8; ++corner)
        {
            builder.add_vertex(
                { corner & 1 ? maximum.x() : minimum.x(),
                  corner & 2 ? maximum.y() : minimum.y(),
                  corner & 4 ? maximum.z() : minimum.z() });
        }
        result<scene::mesh_data> built = std::move(builder).build();
        REQUIRE(built);
        const result<scene::mesh_handle> mesh = assets.create_mesh(std::move(*built));
        REQUIRE(mesh);
        return *mesh;
    }

    void draw(scene::scene& world, const entity entity, const scene::mesh_handle mesh)
    {
        world.registry().emplace<scene::components::mesh>(entity, scene::components::mesh{ .asset = mesh });
    }

    // One tick of 08 §9.6 with trails
    void tick(scene::scene& world, const simulation::simulation_duration time)
    {
        world.update_world_transforms();
        world.update_trails(time);
        world.end_tick();
    }

    std::size_t log_lines_with(const tests::log_capture& capture, const std::string_view text)
    {
        const auto lines = capture.lines();
        return static_cast<std::size_t>(
            std::ranges::count_if(lines, [text](const std::string& line) { return line.contains(text); }));
    }
} // namespace

TEST_CASE("format_label fills names, ids and properties", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const entity car = create(world, 1042, "car 1042");

    auto& values = world.registry().emplace<properties>(car);
    values.set("perception.speed", 8.333, "m/s");
    values.set("perception.class", std::string{ "pedestrian" });
    values.set("perception.tracked", true);
    values.set("perception.velocity", Eigen::Vector3d{ 1.0, 2.0, 3.46 }, "m/s");
    values.set("perception.hits", std::int64_t{ 3 });
    world.registry().emplace<label>(car, label{ .text_template = "{name} · {property:perception.speed:.1f}" });

    tests::log_capture warnings{ "scene" };

    CHECK(world.format_label(car) == "car 1042 · 8.3 m/s");
    CHECK(world.format_label("#{id}", car) == "#1042");
    CHECK(world.format_label("{{braces}} and }} and {{{name}}}", car) == "{braces} and } and {car 1042}");
    CHECK(world.format_label("speed {property:perception.speed}", car) == "speed 8.333 m/s");
    CHECK(world.format_label("{property:perception.class:.1f}", car) == "pedestrian"); // strings ignore the spec
    CHECK(world.format_label("{property:perception.tracked:>8}", car) == "true");
    CHECK(world.format_label("{property:perception.velocity:.1f}", car) == "(1.0, 2.0, 3.5) m/s");
    CHECK(world.format_label("{property:perception.hits}", car) == "3");
    CHECK(world.format_label("{property:perception.hits:.1f}", car) == "3.0"); // as a double
    CHECK(world.format_label("{property:perception.hits:04}", car) == "0003");

    SECTION("a missing key or no properties gives –, without a warning")
    {
        CHECK(world.format_label("{name}: {property:perception.missing}", car) == "car 1042: –");
        const entity bare = create(world, 7, "bare");
        CHECK(world.format_label("{property:perception.speed:.1f}", bare) == "–");
        CHECK(world.format_label(bare).empty()); // no label component
        CHECK(log_lines_with(warnings, "format_label") == 0);
    }

    SECTION("a bad spec gives – and logs once per template")
    {
        for (int call = 0; call < 3; ++call)
        {
            CHECK(world.format_label("{name} {property:perception.speed:q}", car) == "car 1042 –");
        }
        CHECK(log_lines_with(warnings, "format_label") == 1);

        // Another bad template logs again
        CHECK(world.format_label("{nickname}", car) == "–");
        CHECK(world.format_label("open {name", car) == "open –");
        CHECK(log_lines_with(warnings, "format_label") == 3);
    }
}

TEST_CASE("update_trails samples by spacing and drops old samples", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const entity car = create(world, 1, "car", entity_category::vehicle);
    world.registry().emplace<trail>(car, trail{ .settings = { .duration_seconds = 1.0f, .minimum_spacing = 1.0f } });
    const auto samples = [&]() -> const std::deque<trail_sample>& { return world.registry().get<trail>(car).samples; };

    tick(world, 0ms);
    REQUIRE(samples().size() == 1); // the first sample, whatever the spacing
    CHECK(samples()[0].position.isZero());
    CHECK_FALSE(samples()[0].starts_segment);

    world.set_local(car, pose({ 0.5f, 0.0f, 0.0f }));
    tick(world, 100ms);
    CHECK(samples().size() == 1); // moved less than minimum_spacing

    world.set_local(car, pose({ 1.2f, 0.0f, 0.0f }));
    tick(world, 200ms);
    REQUIRE(samples().size() == 2);
    CHECK(samples()[1].position.isApprox(Eigen::Vector3f{ 1.2f, 0.0f, 0.0f }));
    CHECK(samples()[1].time == 200ms);

    SECTION("no update_trails call, no sample")
    {
        world.set_local(car, pose({ 5.0f, 0.0f, 0.0f }));
        world.update_world_transforms();
        world.end_tick();
        CHECK(samples().size() == 2);
        CHECK(world.statistics().trail_samples == 2);
    }

    SECTION("samples older than the duration are dropped")
    {
        for (int step = 3; step <= 12; ++step)
        {
            world.set_local(car, pose({ 1.2f * static_cast<float>(step), 0.0f, 0.0f }));
            tick(world, step * 100ms);
        }
        // Kept: 0.2 s … 1.2 s
        CHECK(samples().front().time == 200ms);
        CHECK(samples().back().time == 1200ms);
        CHECK(samples().size() == 11);
        CHECK(world.statistics().trail_samples == 11);
    }

    SECTION("trails disappear with their entity")
    {
        CHECK(world.statistics().trails == 1);
        world.destroy_entity(car);
        CHECK(world.statistics().trails == 0);
        CHECK(world.statistics().trail_samples == 0);
    }
}

TEST_CASE("a discontinuity starts a new trail segment for the whole subtree", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const entity car = create(world, 1, "car", entity_category::vehicle);
    const entity sensor = create(world, 2, "lidar", entity_category::sensor, car, pose({ 1.0f, 0.0f, 2.0f }));
    world.registry().emplace<trail>(car);
    world.registry().emplace<trail>(sensor);

    tick(world, 0ms);
    world.set_local(car, pose({ 0.2f, 0.0f, 0.0f }), true); // a small jump: below the spacing, still sampled
    tick(world, 100ms);

    for (const entity trailed : { car, sensor })
    {
        const auto& samples = world.registry().get<trail>(trailed).samples;
        REQUIRE(samples.size() == 2);
        CHECK_FALSE(samples[0].starts_segment);
        CHECK(samples[1].starts_segment);
    }

    // The next tick samples normally again
    world.set_local(car, pose({ 5.0f, 0.0f, 0.0f }));
    tick(world, 200ms);
    CHECK_FALSE(world.registry().get<trail>(car).samples.back().starts_segment);
}

TEST_CASE("update_trails checks the tick order and its ownership of the samples", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const entity car = create(world, 1, "car", entity_category::vehicle);
    world.registry().emplace<trail>(car);

    tests::recorded_assertions assertions;
    tests::log_capture errors{ "scene" };

    SECTION("twice in one tick")
    {
        world.update_world_transforms();
        world.update_trails(0ms);
        world.update_trails(0ms);
#ifndef NDEBUG
        CHECK(assertions.contains("update_trails: called twice in one tick"));
#endif
    }

    SECTION("samples written outside scene")
    {
        tick(world, 0ms);
        world.registry().get<trail>(car).samples.push_back({});
        world.update_world_transforms();
        world.update_trails(100ms);
        CHECK(assertions.count() + log_lines_with(errors, "owned by scene") > 0);
#ifndef NDEBUG
        CHECK(assertions.contains("changed outside scene"));
#endif
        // The count follows what's there, so the statistics stay right
        CHECK(world.statistics().trail_samples == world.registry().get<trail>(car).samples.size());
    }

    SECTION("before the transforms are updated")
    {
        world.update_world_transforms();
        world.end_tick();
        world.set_local(car, pose({ 3.0f, 0.0f, 0.0f }));
        world.update_trails(0ms);
#ifndef NDEBUG
        CHECK(assertions.contains("update_trails: a transform changed after the last update_world_transforms"));
#endif
    }
}

TEST_CASE("world_bounds is the union of what the subtree draws", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const scene::mesh_handle body = make_box(assets, { -2.0f, -1.0f, 0.0f }, { 2.0f, 1.0f, 1.5f });
    const scene::mesh_handle frustum = make_box(assets, { 0.0f, -0.5f, -0.5f }, { 3.0f, 0.5f, 0.5f });
    const scene::mesh_handle text = make_box(assets, { -100.0f, -100.0f, -100.0f }, { 100.0f, 100.0f, 100.0f });

    const entity car = create(world, 1, "car", entity_category::vehicle, entt::null, pose({ 10.0f, 5.0f, 0.0f }, 0.7f));
    const entity proxy = create(world, 0, "proxy", entity_category::helper, car);
    const entity front = create(world, 2, "front camera", entity_category::sensor, car, pose({ 2.0f, 0.0f, 1.2f }));
    const entity rear = create(
        world, 3, "rear camera", entity_category::sensor, car, pose({ -2.0f, 0.0f, 1.2f }, std::numbers::pi_v<float>));
    const entity front_frustum = create(world, 0, "front frustum", entity_category::helper, front);
    const entity rear_frustum =
        create(world, 0, "rear frustum", entity_category::helper, rear, pose({ 0.0f, 0.0f, 0.5f }));
    const entity label_helper = create(world, 0, "label", entity_category::helper, car, pose({ 0.0f, 0.0f, 2.0f }));
    draw(world, proxy, body);
    draw(world, front_frustum, frustum);
    draw(world, rear_frustum, frustum);
    draw(world, label_helper, text);
    world.registry().emplace<render_options>(
        label_helper,
        render_options{ .layer = scene::draw_layer::overlay_on_top,
                        .flags = scene::instance_flags::visible | scene::instance_flags::screen_aligned });
    world.update_world_transforms();

    // Brute force over the vertices of what's drawn, the label excluded
    scene::aabb expected;
    for (const entity drawn : { proxy, front_frustum, rear_frustum })
    {
        const auto current = assets.current(world.registry().get<scene::components::mesh>(drawn).asset);
        for (const Eigen::Vector3f& position : current.data->positions)
        {
            expected.extend(world.world(drawn) * position);
        }
    }

    const std::optional<scene::aabb> bounds = world.world_bounds(car);
    REQUIRE(bounds);
    CHECK(bounds->minimum.isApprox(expected.minimum, 1e-5f));
    CHECK(bounds->maximum.isApprox(expected.maximum, 1e-5f));

    // The sensor's subtree only
    const std::optional<scene::aabb> front_bounds = world.world_bounds(front);
    REQUIRE(front_bounds);
    CHECK(front_bounds->maximum.z() < bounds->maximum.z() + 1e-5f);

    // Nothing drawn
    const entity empty = create(world, 9, "empty");
    world.update_world_transforms();
    CHECK_FALSE(world.world_bounds(empty));
    CHECK_FALSE(world.world_bounds(label_helper)); // only a screen-aligned mesh
    const result<entity> settings =
        world.create_entity({ .id = 10, .name = "settings", .category = entity_category::group, .spatial = false });
    REQUIRE(settings);
    CHECK_FALSE(world.world_bounds(*settings));

    world.clear();
    for (const scene::mesh_handle mesh : { body, frustum, text })
    {
        assets.release(mesh);
    }
}

TEST_CASE("world_bounds uses the mesh's primitive range", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    scene::mesh_builder builder{ scene::mesh_topology::points };
    builder.add_vertex({ 0.0f, 0.0f, 0.0f }).add_vertex({ 1.0f, 1.0f, 1.0f }).end_primitive();
    builder.add_vertex({ 10.0f, 10.0f, 10.0f }).add_vertex({ 11.0f, 11.0f, 11.0f }).end_primitive();
    result<scene::mesh_data> built = std::move(builder).build();
    REQUIRE(built);
    const scene::mesh_handle mesh = *assets.create_mesh(std::move(*built));

    const entity drawn = create(world, 1, "drawn");
    world.registry().emplace<scene::components::mesh>(
        drawn, scene::components::mesh{ .asset = mesh, .primitive_first = 1, .primitive_count = 1 });
    world.update_world_transforms();

    const std::optional<scene::aabb> bounds = world.world_bounds(drawn);
    REQUIRE(bounds);
    CHECK(bounds->minimum.isApprox(Eigen::Vector3f::Constant(10.0f)));
    CHECK(bounds->maximum.isApprox(Eigen::Vector3f::Constant(11.0f)));

    world.clear();
    assets.release(mesh);
}

TEST_CASE("reflect lists components in header order, whatever the emplace order", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const scene::mesh_handle body = make_box(assets, { -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });

    const entity car = create(world, 1042, "car 1042", entity_category::object, entt::null, pose({ 3.0f, 0.0f, 0.0f }));
    auto& registry = world.registry();
    registry.emplace<update_stamp>(car, update_stamp{ .created = { .step = 7, .time = 700ms } });
    registry.emplace<trail>(car);
    registry.emplace<label>(car, label{ .text_template = "{name}" });
    registry.emplace<light>(car);
    registry.emplace<visibility>(car);
    registry.emplace<render_options>(car);
    registry.emplace<material>(car);
    draw(world, car, body);
    registry.emplace<properties>(car).set("perception.speed", 2.5, "m/s");
    world.update_world_transforms();

    const scene::reflected_entity reflected = world.reflect(car);
    CHECK(reflected.id == 1042);
    CHECK(reflected.name == "car 1042");

    std::vector<std::string_view> names;
    for (const scene::reflected_component& component : reflected.components)
    {
        names.push_back(component.name);
    }
    const std::vector<std::string_view> expected{
        scene::reflection<identity>::name,     scene::reflection<scene::components::mesh>::name,
        scene::reflection<material>::name,     scene::reflection<render_options>::name,
        scene::reflection<visibility>::name,   scene::reflection<light>::name,
        scene::reflection<label>::name,        scene::reflection<trail>::name,
        scene::reflection<update_stamp>::name,
    };
    CHECK(names == expected);

    REQUIRE(reflected.properties.size() == 1);
    CHECK(reflected.properties[0].key == "perception.speed");
    CHECK(reflected.stamp.created.step == 7);
    REQUIRE(reflected.world_bounds);
    CHECK(reflected.world_bounds->minimum.isApprox(Eigen::Vector3f{ 2.0f, -1.0f, -1.0f }));

    // Without an update_stamp the stamp is the default
    const entity bare = create(world, 5, "bare");
    world.update_world_transforms();
    const scene::reflected_entity plain = world.reflect(bare);
    CHECK(plain.stamp.created.step == 0);
    CHECK_FALSE(plain.world_bounds);
    REQUIRE(plain.components.size() == 1);
    CHECK(plain.components[0].name == scene::reflection<identity>::name);

    world.clear();
    assets.release(body);
}
