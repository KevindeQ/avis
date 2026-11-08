// The scene representation's display shapes and helpers (docs/tasks/12-visualizer.md §12.8): box faces and edges as
// helper children with `represents`, the extent on the child's scale, the ghost vehicle, frustums, the selection and
// visibility flags through the simulation loop, helpers destroyed with their entity, and the material cache.

#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"
#include "representation/presentation_state.h"
#include "representation/representation_system.h"
#include "representation/shared_assets.h"
#include "simulation/demo_scene.h"
#include "simulation/gui_sink.h"
#include "simulation/simulation_command.h"
#include "simulation/simulation_thread.h"

#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/scene.h"
#include "avis/scene/snapshot.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using avis::result;
namespace representation = avis::visualizer::representation;
namespace scene = avis::scene;
namespace simulation = avis::visualizer::simulation;
namespace tests = avis::tests;
using namespace avis::scene::components;
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr scene::object_id driver_id(const std::uint64_t index) noexcept
    {
        return (std::uint64_t{ 1 } << 56) | index;
    }

    display_shape
        box_shape(const std::array<std::uint8_t, 4> colour, const Eigen::Vector3f& extent = { 4.0f, 2.0f, 1.5f })
    {
        display_shape shape;
        shape.shape = shape_kind::box;
        shape.extent = extent;
        shape.shape_color = scene::color::from_srgb8(colour);
        shape.opacity = 0.5f;
        return shape;
    }

    // A scene and a representation_system, updated as step 4 would be
    struct representation_fixture
    {
        representation_fixture()
        {
            result<std::unique_ptr<representation::representation_system>> created =
                representation::representation_system::create(assets);
            REQUIRE(created);
            system = std::move(*created);
            state.grid = false; // only the grid's test wants the helpers
            state.axes_gizmo = false;
        }

        void update()
        {
            world.update_world_transforms();
            system->update(world, state);
            world.update_world_transforms();
        }

        scene::entity
            add(const std::uint64_t index,
                const display_shape& shape,
                const scene::entity parent = entt::null,
                const scene::entity_category category = scene::entity_category::object)
        {
            const result<scene::entity> created = world.create_entity(
                { .id = driver_id(index),
                  .name = "entity " + std::to_string(index),
                  .category = category,
                  .parent = parent });
            REQUIRE(created);
            world.registry().emplace<driver_entity>(*created, driver_entity{ .category = category });
            world.registry().emplace<display_shape>(*created, shape);
            return *created;
        }

        const scene::material_description& material_of(const scene::entity part, const std::size_t index = 0) const
        {
            const scene::material_handle handle = world.registry().get<material>(part).materials.at(index);
            const scene::versioned_material current = assets.current(handle);
            REQUIRE(current.data);
            return *current.data;
        }

        // Declared in destruction order: the system disconnects from the live scene, then the scene releases the
        // parts' assets, then the cache checks every slot is free
        scene::asset_cache assets;
        scene::scene world{ assets };
        std::unique_ptr<representation::representation_system> system;
        representation::presentation_state state = representation::presentation_state::from_settings({});
    };

    class quiet_sink final : public simulation::gui_sink
    {
    public:
        void tree_changed(std::vector<scene::tree_change>) override {}
        void selected_entity_changed(std::optional<scene::reflected_entity>) override {}
        void statistics_changed(const scene::scene_statistics&) override {}
        void follow_target_changed(const simulation::follow_target&) override {}
    };
} // namespace

TEST_CASE("representation: a box gets faces in world and edges in overlay", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    representation_fixture fixture;
    const scene::entity box = fixture.add(1, box_shape({ 200, 60, 60, 255 }));
    fixture.update();

    const std::span<const scene::entity> parts = fixture.system->parts(box);
    REQUIRE(parts.size() == 2);
    const scene::entity faces = parts[0];
    const scene::entity edges = parts[1];
    entt::registry& registry = fixture.world.registry();
    for (const scene::entity part : parts)
    {
        REQUIRE(registry.all_of<represents, mesh, material, render_options>(part));
        CHECK(registry.get<represents>(part).owner == box);
        CHECK(fixture.world.selection_target(part) == box);
        CHECK(registry.get<identity>(part).category == scene::entity_category::helper);
        CHECK(scene::is_helper(registry.get<identity>(part).id));
    }

    CHECK(registry.get<mesh>(faces).asset == fixture.system->shared().unit_cube());
    CHECK(registry.get<render_options>(faces).layer == scene::draw_layer::world);
    CHECK(fixture.material_of(faces).alpha == scene::alpha_mode::blend);
    CHECK_THAT(fixture.material_of(faces).base_color_factor.alpha, WithinAbs(0.5, 1e-6));

    CHECK(registry.get<mesh>(edges).asset == fixture.system->shared().cube_edges());
    CHECK(registry.get<render_options>(edges).layer == scene::draw_layer::overlay);
    CHECK(fixture.material_of(edges).shading == scene::shading_model::unlit);
    CHECK(fixture.material_of(edges).line_width == fixture.state.representation.boxes.edge_width);

    // The scene tree never shows the parts: their `added` changes are hidden
    for (const scene::tree_change& change : fixture.world.take_tree_changes())
    {
        CHECK(change.hidden == scene::is_helper(change.id));
    }

    // Both reach the snapshot, as the owner's children
    scene::snapshot snapshot;
    fixture.world.make_snapshot(snapshot);
    std::set<scene::object_id> drawn;
    for (const scene::render_instance& instance : snapshot.instances)
    {
        drawn.insert(instance.object_id);
    }
    CHECK(drawn == std::set{ registry.get<identity>(faces).id, registry.get<identity>(edges).id });
    CHECK(assertions.count() == 0);
}

TEST_CASE("representation: the extent scales the child, never the entity", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    representation_fixture fixture;
    const scene::entity box = fixture.add(1, box_shape({ 200, 60, 60, 255 }));
    fixture.update();
    const std::vector<scene::entity> before(fixture.system->parts(box).begin(), fixture.system->parts(box).end());

    fixture.world.registry().patch<display_shape>(
        box, [](display_shape& shape) { shape.extent = { 6.0f, 2.5f, 3.0f }; });
    fixture.update();

    // The same parts, rescaled; the entity keeps unit scale (debug builds assert it on Driver entities)
    const std::span<const scene::entity> parts = fixture.system->parts(box);
    CHECK(std::ranges::equal(parts, before));
    for (const scene::entity part : parts)
    {
        CHECK(fixture.world.local(part).scale.isApprox(Eigen::Vector3f{ 6.0f, 2.5f, 3.0f }));
    }
    CHECK(fixture.world.local(box).scale == Eigen::Vector3f::Ones());

    // The world bounds follow the drawn size
    const std::optional<scene::aabb> bounds = fixture.world.world_bounds(box);
    REQUIRE(bounds);
    CHECK(bounds->maximum.isApprox(Eigen::Vector3f{ 3.0f, 1.25f, 1.5f }, 1e-5f));
    CHECK(assertions.count() == 0);
}

TEST_CASE("representation: vehicles, axes and frustums", "[visualizer][unit]")
{
    representation_fixture fixture;
    display_shape solid;
    solid.shape = shape_kind::vehicle;
    solid.extent = { 4.6f, 1.9f, 1.5f };
    display_shape ghost = solid;
    ghost.opacity = 0.5f;
    const scene::entity ego = fixture.add(1, solid, entt::null, scene::entity_category::vehicle);
    const scene::entity shadow = fixture.add(2, ghost, entt::null, scene::entity_category::vehicle);

    display_shape camera;
    camera.shape = shape_kind::frustum;
    camera.horizontal_fov = 1.4f;
    camera.vertical_fov = 0.5f;
    camera.range = 8.0f;
    const scene::entity image = fixture.add(3, camera, ego, scene::entity_category::sensor);
    display_shape axes;
    axes.shape = shape_kind::axes;
    axes.extent = Eigen::Vector3f::Constant(0.5f);
    const scene::entity lidar = fixture.add(4, axes, ego, scene::entity_category::sensor);
    fixture.update();

    entt::registry& registry = fixture.world.registry();

    SECTION("a vehicle with opacity 0.5 uses the ghost material")
    {
        REQUIRE(fixture.system->parts(ego).size() == 1);
        REQUIRE(fixture.system->parts(shadow).size() == 1);
        const scene::entity body = fixture.system->parts(ego)[0];
        const scene::entity ghost_body = fixture.system->parts(shadow)[0];
        CHECK(registry.get<mesh>(body).asset == fixture.system->shared().vehicle_proxy());
        CHECK(fixture.material_of(body).alpha == scene::alpha_mode::opaque);
        CHECK(fixture.material_of(body, 1).debug_name == "vehicle wheels");
        CHECK(fixture.material_of(ghost_body).alpha == scene::alpha_mode::blend);
        CHECK(fixture.material_of(ghost_body).debug_name == "ghost vehicle");
        CHECK_THAT(fixture.material_of(ghost_body).base_color_factor.alpha, WithinAbs(0.5, 1e-6));
        CHECK(fixture.material_of(ghost_body, 1).debug_name == "ghost vehicle"); // the wheels too

        // The proxy stands on the ground, forward of the rear axle
        CHECK(fixture.world.local(body).position.isApprox(Eigen::Vector3f{ 0.3f * 4.6f, 0.0f, 0.0f }));
        CHECK(registry.get<render_options>(body).flags.count(scene::instance_flags::casts_shadows) == 1);
        CHECK(registry.get<render_options>(ghost_body).flags.count(scene::instance_flags::casts_shadows) == 0);
    }

    SECTION("axes and frustums are overlay lines; a frustum's mesh follows its angles and range")
    {
        REQUIRE(fixture.system->parts(lidar).size() == 1);
        const scene::entity axes_part = fixture.system->parts(lidar)[0];
        CHECK(registry.get<mesh>(axes_part).asset == fixture.system->shared().axes());
        CHECK(registry.get<render_options>(axes_part).layer == scene::draw_layer::overlay);
        CHECK(fixture.world.local(axes_part).scale.isApprox(Eigen::Vector3f::Constant(0.5f)));

        REQUIRE(fixture.system->parts(image).size() == 1);
        const scene::entity frustum = fixture.system->parts(image)[0];
        CHECK(registry.get<render_options>(frustum).layer == scene::draw_layer::overlay);
        const scene::mesh_handle handle = registry.get<mesh>(frustum).asset;
        const scene::versioned_mesh first = fixture.assets.current(handle);
        REQUIRE(first.data);
        CHECK(first.data->topology == scene::mesh_topology::lines);
        CHECK(first.data->indices.size() == 16);
        CHECK_THAT(first.data->bounds.maximum.x(), WithinAbs(8.0, 1e-5));

        // A change of a sibling component isn't a change of the frustum: same version
        fixture.update();
        CHECK(fixture.assets.current(handle).version == first.version);

        registry.patch<display_shape>(image, [](display_shape& shape) { shape.range = 20.0f; });
        fixture.update();
        CHECK(registry.get<mesh>(frustum).asset == handle);
        const scene::versioned_mesh second = fixture.assets.current(handle);
        CHECK(second.version == first.version + 1);
        CHECK_THAT(second.data->bounds.maximum.x(), WithinAbs(20.0, 1e-5));
    }

    SECTION("another shape kind replaces the parts, and none removes them")
    {
        registry.patch<display_shape>(lidar, [](display_shape& shape) { shape.shape = shape_kind::box; });
        fixture.update();
        CHECK(fixture.system->parts(lidar).size() == 2);

        registry.remove<display_shape>(lidar);
        fixture.update();
        CHECK(fixture.system->parts(lidar).empty());
        CHECK(registry.view<represents>().size() == 3); // the two vehicles' bodies and the frustum
    }
}

TEST_CASE("representation: destroying the entity destroys its helpers", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    representation_fixture fixture;
    const scene::entity box = fixture.add(1, box_shape({ 200, 60, 60, 255 }));
    const scene::entity other = fixture.add(2, box_shape({ 60, 200, 60, 255 }));
    fixture.update();
    const std::vector<scene::entity> parts(fixture.system->parts(box).begin(), fixture.system->parts(box).end());
    REQUIRE(parts.size() == 2);
    const std::uint64_t meshes_before = fixture.assets.statistics().meshes;

    static_cast<void>(fixture.world.destroy_entity(box));
    for (const scene::entity part : parts)
    {
        CHECK_FALSE(fixture.world.alive(part));
    }
    fixture.update();
    CHECK(fixture.system->parts(box).empty());
    CHECK(fixture.system->parts(other).size() == 2);
    CHECK(fixture.world.registry().view<represents>().size() == 2);
    CHECK(fixture.assets.statistics().meshes == meshes_before); // shared meshes: nothing to free
    CHECK(assertions.count() == 0);
}

TEST_CASE("representation: materials are cached by colour and opacity", "[visualizer][unit]")
{
    representation_fixture fixture;
    constexpr std::array<std::array<std::uint8_t, 4>, 3> colours{
        { { 220, 60, 60, 255 }, { 60, 200, 60, 255 }, { 60, 90, 230, 255 } }
    };
    std::vector<scene::entity> boxes;
    for (std::uint64_t index = 0; index < 30; ++index)
    {
        boxes.push_back(fixture.add(index + 1, box_shape(colours[index % 3])));
    }
    fixture.update();

    std::set<scene::material_handle> faces;
    std::set<scene::material_handle> edges;
    for (const scene::entity box : boxes)
    {
        REQUIRE(fixture.system->parts(box).size() == 2);
        faces.insert(fixture.world.registry().get<material>(fixture.system->parts(box)[0]).materials.front());
        edges.insert(fixture.world.registry().get<material>(fixture.system->parts(box)[1]).materials.front());
    }
    CHECK(faces.size() == 3);
    CHECK(edges.size() == 3);
    CHECK(fixture.system->materials().size() == 6);

    // Another opacity is another face material, but the same edges (they're opaque)
    fixture.world.registry().patch<display_shape>(boxes[0], [](display_shape& shape) { shape.opacity = 0.8f; });
    fixture.update();
    CHECK(fixture.system->materials().size() == 7);

    // A new edge width: new edge materials, every box rebuilt with them
    fixture.state.representation.boxes.edge_width = 4.0f;
    fixture.update();
    CHECK(fixture.material_of(fixture.system->parts(boxes[1])[1]).line_width == 4.0f);
    CHECK(fixture.system->materials().size() == 7);
}

TEST_CASE("representation: the grid and the axes gizmo follow their toggles", "[visualizer][unit]")
{
    representation_fixture fixture;
    const result<scene::entity> group = fixture.world.create_entity(
        { .name = representation::helpers_group_name, .category = scene::entity_category::helper });
    REQUIRE(group);
    fixture.state.grid = true;
    fixture.state.axes_gizmo = true;
    fixture.update();

    entt::registry& registry = fixture.world.registry();
    const scene::entity grid = fixture.system->grid();
    const scene::entity gizmo = fixture.system->axes_gizmo();
    REQUIRE(fixture.world.alive(grid));
    REQUIRE(fixture.world.alive(gizmo));
    CHECK(registry.get<render_options>(grid).layer == scene::draw_layer::overlay);
    CHECK(registry.get<render_options>(gizmo).layer == scene::draw_layer::overlay_on_top);
    CHECK(registry.get<render_options>(grid).flags.count(scene::instance_flags::pickable) == 0);
    CHECK(registry.get<mesh>(grid).asset == fixture.system->shared().grid());
    CHECK(fixture.material_of(grid).alpha == scene::alpha_mode::blend);

    // Under the helpers group, so Reset keeps them
    scene::snapshot snapshot;
    fixture.world.make_snapshot(snapshot);
    CHECK(snapshot.instances.size() == 2);

    // The grid fades out: vertex alpha falls with the distance from the origin
    const scene::versioned_mesh lines = fixture.assets.current(fixture.system->shared().grid());
    REQUIRE(lines.data);
    std::uint8_t near_alpha = 0;
    std::uint8_t far_alpha = 255;
    for (std::size_t vertex = 0; vertex < lines.data->positions.size(); ++vertex)
    {
        const float distance = lines.data->positions[vertex].norm();
        if (distance < 2.0f)
        {
            near_alpha = std::max(near_alpha, lines.data->color0[vertex][3]);
        }
        if (distance > 0.95f * representation::major_grid_radius)
        {
            far_alpha = std::min(far_alpha, lines.data->color0[vertex][3]);
        }
    }
    CHECK(near_alpha > 50);
    CHECK(far_alpha < 5);

    fixture.state.grid = false;
    fixture.update();
    CHECK_FALSE(fixture.world.alive(grid));
    CHECK(fixture.world.alive(gizmo));

    fixture.state.grid = true;
    fixture.state.axes_gizmo = false;
    fixture.update();
    CHECK(fixture.world.alive(fixture.system->grid()));
    CHECK_FALSE(fixture.world.alive(gizmo));
}

TEST_CASE("representation: the loop's visibility and selection flags reach every part", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    using clock_type = simulation::simulation_loop::clock;
    clock_type::time_point now = clock_type::time_point{ 1h };

    scene::asset_cache assets;
    scene::snapshot_buffer snapshots;
    quiet_sink sink;
    result<std::unique_ptr<representation::representation_system>> created =
        representation::representation_system::create(assets);
    REQUIRE(created);
    const std::unique_ptr<representation::representation_system> system = std::move(*created);
    std::optional<simulation::simulation_loop> loop;
    loop.emplace(assets, snapshots, sink, simulation::simulation_settings{}, now, system.get());

    // A box and a vehicle, created as the scene stream would, between republishes
    scene::scene& world = loop->scene();
    const auto add = [&](const std::uint64_t index, const display_shape& shape)
    {
        const result<scene::entity> entity = world.create_entity(
            { .id = driver_id(index), .name = "entity", .category = scene::entity_category::object });
        REQUIRE(entity);
        world.registry().emplace<driver_entity>(*entity);
        world.registry().emplace<display_shape>(*entity, shape);
        return *entity;
    };
    const scene::entity box = add(1, box_shape({ 220, 60, 60, 255 }));
    display_shape vehicle_shape;
    vehicle_shape.shape = shape_kind::vehicle;
    const scene::entity car = add(2, vehicle_shape);

    // Each command is republished 16 ms later
    const auto run = [&](const simulation::simulation_command& command)
    {
        loop->handle(command, now);
        now += 20ms;
        loop->update(now);
    };
    run(simulation::select_entity{ .id = driver_id(1) });

    // The flags of every instance an entity's parts draw
    const auto flags_of = [&](const scene::entity owner)
    {
        std::vector<avis::flags<scene::instance_flags>> found;
        const scene::snapshot_reference reference = snapshots.acquire();
        REQUIRE(reference);
        for (const scene::entity part : system->parts(owner))
        {
            const scene::object_id id = world.registry().get<identity>(part).id;
            for (const scene::render_instance& instance : reference->instances)
            {
                if (instance.object_id == id)
                {
                    found.push_back(instance.flags);
                }
            }
        }
        return found;
    };
    const auto all = [](const std::vector<avis::flags<scene::instance_flags>>& flags, const scene::instance_flags flag)
    { return !flags.empty() && std::ranges::all_of(flags, [&](const auto value) { return value.count(flag) != 0; }); };
    const auto none = [](const std::vector<avis::flags<scene::instance_flags>>& flags, const scene::instance_flags flag)
    { return !flags.empty() && std::ranges::none_of(flags, [&](const auto value) { return value.count(flag) != 0; }); };

    CHECK(flags_of(box).size() == 2);
    CHECK(flags_of(car).size() == 2); // body and wheels
    CHECK(all(flags_of(box), scene::instance_flags::selected));
    CHECK(none(flags_of(car), scene::instance_flags::selected));
    CHECK(all(flags_of(box), scene::instance_flags::visible));

    // Selecting a part selects its owner
    const scene::object_id body_id = world.registry().get<identity>(system->parts(car)[0]).id;
    run(simulation::select_entity{ .id = body_id });
    CHECK(loop->presentation().selected == driver_id(2));
    CHECK(none(flags_of(box), scene::instance_flags::selected));
    CHECK(all(flags_of(car), scene::instance_flags::selected));

    run(simulation::set_entity_visibility{ .id = driver_id(1), .visible = false });
    CHECK(none(flags_of(box), scene::instance_flags::visible));
    CHECK(all(flags_of(car), scene::instance_flags::visible));

    run(simulation::set_entity_visibility{ .id = driver_id(1), .visible = true });
    CHECK(all(flags_of(box), scene::instance_flags::visible));

    loop.reset(); // detaches the representation before its scene goes
    CHECK(assertions.count() == 0);
}

TEST_CASE("representation: the demo scene runs with every shape drawn", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    tests::log_capture scene_log{ "scene", 64, spdlog::level::err };
    tests::log_capture visualizer_log{ "visualizer", 64, spdlog::level::warn };
    using clock_type = simulation::simulation_loop::clock;
    clock_type::time_point now = clock_type::time_point{ 1h };

    scene::asset_cache assets;
    scene::snapshot_buffer snapshots;
    quiet_sink sink;
    result<std::unique_ptr<representation::representation_system>> created =
        representation::representation_system::create(assets);
    REQUIRE(created);
    const std::unique_ptr<representation::representation_system> system = std::move(*created);
    {
        simulation::simulation_settings settings;
        settings.demo_scene = true;
        simulation::simulation_loop loop{ assets, snapshots, sink, settings, now, system.get() };
        for (int tick = 0; tick < 200; ++tick)
        {
            loop.update(now);
            now += simulation::demo_scene::tick_interval;
        }

        // Every display shape is drawn, and every part belongs to a live entity with one
        scene::scene& world = loop.scene();
        entt::registry& registry = world.registry();
        for (const auto [entity, shape] : registry.view<display_shape>().each())
        {
            CHECK(system->parts(entity).size() == (shape.shape == shape_kind::box ? 2u : 1u));
        }
        for (const auto [part, owner] : registry.view<represents>().each())
        {
            CHECK(registry.all_of<display_shape>(owner.owner));
        }
        CHECK(world.alive(system->grid()));
        CHECK(world.alive(system->axes_gizmo()));

        // The boxes' three colours make three face and three edge materials; the vehicles, ghost and frustums 3 more
        CHECK(system->materials().size() == 9);

        // Reset keeps the grid and the gizmo, under the helpers group; the demo's next tick draws its entities again
        loop.handle(simulation::reset_scene{}, now);
        loop.update(now);
        CHECK(world.alive(system->grid()));
        CHECK(world.alive(system->axes_gizmo()));
        for (const auto [part, owner] : registry.view<represents>().each())
        {
            CHECK(world.alive(owner.owner));
        }
    }
    CHECK(assertions.count() == 0);
    CHECK(scene_log.lines().empty());
    CHECK(visualizer_log.lines().empty());
}
