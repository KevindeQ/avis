// scene: hierarchy, transforms, discontinuities, previous_world and the tick order
// (docs/tasks/10-scene-management.md §10.10)

#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"

#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/spatial.h"
#include "avis/scene/error.h"
#include "avis/scene/scene.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <format>
#include <initializer_list>
#include <numbers>
#include <vector>

using namespace avis;
using namespace avis::scene::components;
using avis::scene::entity;
using avis::scene::entity_category;
using avis::scene::transform;

namespace
{
    entity create(
        scene::scene& world,
        const scene::object_id id,
        const entity_category category,
        const entity parent = entt::null,
        const transform& local = {})
    {
        const result<entity> created = world.create_entity(
            { .id = id, .name = std::format("entity {}", id), .category = category, .parent = parent, .local = local });
        REQUIRE(created);
        return *created;
    }

    transform pose(const Eigen::Vector3f& position, const float yaw)
    {
        transform made;
        made.position = position;
        made.rotation = Eigen::Quaternionf{ Eigen::AngleAxisf{ yaw, Eigen::Vector3f::UnitZ() } };
        return made;
    }

    const Eigen::Affine3f& previous(scene::scene& world, const entity entity)
    {
        REQUIRE(world.registry().all_of<previous_world>(entity));
        return world.registry().get<previous_world>(entity).world;
    }
} // namespace

TEST_CASE("a discontinuity stops the whole subtree from interpolating for one tick", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const entity vehicle = create(world, 1, entity_category::vehicle, entt::null, pose({ 10.0f, 0.0f, 0.0f }, 0.0f));
    const entity sensor = create(world, 2, entity_category::sensor, vehicle, pose({ 1.5f, 0.0f, 1.8f }, 0.0f));
    const entity frustum = create(world, 0, entity_category::helper, sensor, pose({ 0.0f, 0.0f, 0.1f }, 0.0f));
    const entity other = create(world, 3, entity_category::object, entt::null, pose({ -5.0f, 0.0f, 0.0f }, 0.0f));

    // Tick 1
    world.update_world_transforms();
    world.end_tick();

    // Tick 2: the vehicle jumps, the other object moves normally
    world.set_local(vehicle, pose({ 500.0f, 20.0f, 0.0f }, 1.0f), true);
    world.set_local(other, pose({ -4.0f, 0.0f, 0.0f }, 0.0f));
    world.update_world_transforms();

    for (const entity jumped : { vehicle, sensor, frustum })
    {
        CHECK(previous(world, jumped).isApprox(world.world(jumped)));
    }
    CHECK(previous(world, other).translation().isApprox(Eigen::Vector3f{ -5.0f, 0.0f, 0.0f }));
    CHECK_FALSE(previous(world, other).isApprox(world.world(other)));
    world.end_tick();

    // Tick 3: an ordinary move interpolates from the tick-2 world again
    const Eigen::Affine3f tick_2_world = world.world(sensor);
    world.set_local(vehicle, pose({ 501.0f, 20.0f, 0.0f }, 1.0f));
    world.update_world_transforms();
    CHECK(previous(world, sensor).isApprox(tick_2_world));
    CHECK_FALSE(previous(world, sensor).isApprox(world.world(sensor)));
    world.end_tick();
}

TEST_CASE("previous_world holds the world of the last finished tick", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const entity car = create(world, 1, entity_category::object, entt::null, pose({ 1.0f, 2.0f, 0.0f }, 0.5f));
    world.update_world_transforms();
    CHECK_FALSE(world.registry().all_of<previous_world>(car)); // created this tick
    world.end_tick();
    CHECK(previous(world, car).isApprox(world.world(car)));

    // An entity created in a later tick has none until that tick ends
    world.set_local(car, pose({ 2.0f, 2.0f, 0.0f }, 0.5f));
    const entity late = create(world, 2, entity_category::object, car);
    world.update_world_transforms();
    CHECK_FALSE(world.registry().all_of<previous_world>(late));
    CHECK(previous(world, car).translation().isApprox(Eigen::Vector3f{ 1.0f, 2.0f, 0.0f }));
    world.end_tick();
    CHECK(previous(world, late).isApprox(world.world(late)));
    CHECK(previous(world, car).translation().isApprox(Eigen::Vector3f{ 2.0f, 2.0f, 0.0f }));

    // A jump on an entity created this tick emplaces previous_world straight away: it doesn't pop in twice
    const entity jumper = create(world, 3, entity_category::object);
    world.set_local(jumper, pose({ 9.0f, 0.0f, 0.0f }, 0.0f), true);
    world.update_world_transforms();
    CHECK(previous(world, jumper).isApprox(world.world(jumper)));
    world.end_tick();
}

TEST_CASE("local, world and relative_transform", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const transform vehicle_pose = pose({ 100.0f, -20.0f, 0.5f }, std::numbers::pi_v<float> / 3.0f);
    transform mount = pose({ 1.2f, 0.4f, 1.7f }, -0.25f);
    mount.rotation = mount.rotation * Eigen::Quaternionf{ Eigen::AngleAxisf{ 0.1f, Eigen::Vector3f::UnitY() } };

    const entity vehicle = create(world, 1, entity_category::vehicle, entt::null, vehicle_pose);
    const entity sensor = create(world, 2, entity_category::sensor, vehicle, mount);
    world.update_world_transforms();

    CHECK(world.local(sensor).position.isApprox(mount.position));
    CHECK(world.world(sensor).isApprox(vehicle_pose.to_affine() * mount.to_affine(), 1e-5f));
    CHECK(world.relative_transform(vehicle, sensor).isApprox(mount.to_affine(), 1e-5f));
    CHECK(world.relative_transform(sensor, sensor).isApprox(Eigen::Affine3f::Identity(), 1e-5f));

    SECTION("an entity without a node is a debug check and gives the identity")
    {
        const result<entity> settings = world.create_entity({ .id = 9, .spatial = false });
        REQUIRE(settings);

        tests::recorded_assertions assertions;
        tests::log_capture errors{ "scene" };
        CHECK(world.world(*settings).isApprox(Eigen::Affine3f::Identity()));
        CHECK(world.local(*settings).position.isZero());
        world.set_local(*settings, vehicle_pose); // does nothing
#ifndef NDEBUG
        CHECK(assertions.count() == 3);
        CHECK(assertions.contains("world: the entity isn't alive or has no node"));
#endif
    }
}

TEST_CASE("set_parent moves entities and records the change", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const entity first = create(world, 1, entity_category::vehicle, entt::null, pose({ 10.0f, 0.0f, 0.0f }, 0.0f));
    const entity second = create(world, 2, entity_category::vehicle, entt::null, pose({ 0.0f, 10.0f, 0.0f }, 1.0f));
    const entity child = create(world, 3, entity_category::sensor, first, pose({ 1.0f, 0.0f, 0.0f }, 0.0f));
    world.update_world_transforms();
    world.take_tree_changes();

    SECTION("keep_world keeps the world transform")
    {
        const Eigen::Affine3f before = world.world(child);
        const std::uint64_t version = world.scene_version();
        REQUIRE(world.set_parent(child, second, true));
        world.update_world_transforms();
        CHECK(world.world(child).isApprox(before, 1e-5f));
        CHECK(world.scene_version() == version + 1);

        const std::vector<scene::tree_change> changes = world.take_tree_changes();
        REQUIRE(changes.size() == 1);
        CHECK(changes[0].kind == scene::tree_change_kind::moved);
        CHECK(changes[0].id == 3);
        CHECK(changes[0].parent == 2);
    }

    SECTION("without keep_world the local transform stays")
    {
        REQUIRE(world.set_parent(child, second, false));
        world.update_world_transforms();
        CHECK(world.relative_transform(second, child).isApprox(world.local(child).to_affine(), 1e-5f));
    }

    SECTION("a null parent makes a root")
    {
        REQUIRE(world.set_parent(child, entt::null, true));
        world.update_world_transforms();
        CHECK(world.world(child).translation().isApprox(Eigen::Vector3f{ 11.0f, 0.0f, 0.0f }));
        const std::vector<scene::tree_change> changes = world.take_tree_changes();
        REQUIRE(changes.size() == 1);
        CHECK(changes[0].parent == 0);
    }

    SECTION("a cycle, a dead parent and a non-spatial parent fail and change nothing")
    {
        const result<void> cycle = world.set_parent(first, child, false);
        REQUIRE_FALSE(cycle);
        CHECK(scene::is_error(cycle.error(), scene::error_kind::cycle));

        const entity dead = create(world, 4, entity_category::object);
        world.destroy_entity(dead);
        const result<void> to_dead = world.set_parent(child, dead, false);
        REQUIRE_FALSE(to_dead);
        CHECK(scene::is_error(to_dead.error(), scene::error_kind::invalid_parent));

        const result<entity> settings = world.create_entity({ .id = 5, .spatial = false });
        REQUIRE(settings);
        const result<void> to_settings = world.set_parent(child, *settings, false);
        REQUIRE_FALSE(to_settings);
        CHECK(scene::is_error(to_settings.error(), scene::error_kind::invalid_parent));

        world.take_tree_changes();
        world.update_world_transforms();
        CHECK(world.relative_transform(first, child).translation().isApprox(Eigen::Vector3f{ 1.0f, 0.0f, 0.0f }));
        CHECK(world.take_tree_changes().empty());
    }
}

TEST_CASE("a Driver entity's node keeps unit scale", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const entity car = create(world, 1042, entity_category::object);
    world.registry().emplace<driver_entity>(car, driver_entity{ .category = entity_category::object });
    const entity helper = create(world, 0, entity_category::helper);

    transform scaled;
    scaled.scale = Eigen::Vector3f::Constant(2.0f);

    tests::recorded_assertions assertions;
    tests::log_capture errors{ "scene" };

    world.set_local(helper, scaled);
    CHECK(assertions.count() == 0);

    transform almost_unit;
    almost_unit.scale = Eigen::Vector3f::Constant(1.00005f);
    world.set_local(car, almost_unit);
    CHECK(assertions.count() == 0);

    world.set_local(car, scaled);
#ifndef NDEBUG
    CHECK(assertions.contains("must have unit scale"));
#endif
}

TEST_CASE("the tick order is checked in debug builds", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const entity car = create(world, 1, entity_category::object);
    const entity helper = create(world, 0, entity_category::helper, car);

    tests::recorded_assertions assertions;
    tests::log_capture errors{ "scene" };

    SECTION("the tick sequence and a republish don't assert")
    {
        for (int tick = 0; tick < 3; ++tick)
        {
            world.set_local(car, pose({ static_cast<float>(tick), 0.0f, 0.0f }, 0.0f)); // 1. messages
            world.update_world_transforms(); // 2.
            world.set_local(helper, pose({ 0.0f, 0.0f, static_cast<float>(tick) }, 0.0f)); // 4. representation
            world.update_world_transforms(); // 5.
            world.end_tick(); // 7.

            // A presentation-only republish: steps 4–6, no end_tick
            world.set_local(helper, pose({ 0.0f, 1.0f, 0.0f }, 0.0f));
            world.update_world_transforms();
        }

        // A tick with nothing but the transform update
        world.update_world_transforms();
        world.end_tick();
        CHECK(assertions.count() == 0);
    }

    SECTION("end_tick twice")
    {
        world.update_world_transforms();
        world.end_tick();
        world.end_tick();
#ifndef NDEBUG
        CHECK(assertions.count() == 1);
        CHECK(assertions.contains("end_tick: called twice"));
#endif
    }

    SECTION("end_tick after an unpropagated set_local")
    {
        world.update_world_transforms();
        world.set_local(car, pose({ 3.0f, 0.0f, 0.0f }, 0.0f));
        world.end_tick();
#ifndef NDEBUG
        CHECK(assertions.contains("end_tick: a transform changed after the last update_world_transforms"));
#endif

        // It propagates the change itself, so previous_world still gets the current world
        CHECK(previous(world, car).translation().isApprox(Eigen::Vector3f{ 3.0f, 0.0f, 0.0f }));
    }
}

TEST_CASE("end_tick on 10,000 entities takes under 0.1 ms", "[scene][unit][!benchmark][.]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    std::vector<entity> roots;
    for (scene::object_id id = 1; id <= 10'000; ++id)
    {
        const entity parent = id % 10 == 1 ? entt::null : roots.back();
        const entity created = create(world, id, entity_category::object, parent, pose({ 1.0f, 0.0f, 0.0f }, 0.0f));
        if (id % 10 == 1)
        {
            roots.push_back(created);
        }
    }
    world.update_world_transforms();
    world.end_tick(); // emplaces every previous_world

    constexpr int iterations = 200;
    std::chrono::nanoseconds total{};
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        world.update_world_transforms();
        const auto start = std::chrono::steady_clock::now();
        world.end_tick();
        total += std::chrono::steady_clock::now() - start;
    }

    const double average = std::chrono::duration<double, std::milli>(total).count() / iterations;
    WARN("average end_tick over 10,000 entities: " << average << " ms");
#ifdef NDEBUG
    CHECK(average < 0.1);
#endif
}
