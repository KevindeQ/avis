// scene: entities, ids, subtree destruction, selection targets, asset lifetime, owned components, tree changes and the
// thread check (docs/tasks/10-scene-management.md §10.9)

#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"

#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/components/spatial.h"
#include "avis/scene/components/viewing.h"
#include "avis/scene/error.h"
#include "avis/scene/scene.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace avis;
using namespace avis::scene::components;
using avis::scene::entity;
using avis::scene::entity_category;
using avis::scene::entity_description;
using avis::scene::tree_change_kind;

namespace
{
    entity create(
        scene::scene& world,
        const scene::object_id id,
        const entity_category category,
        const entity parent = entt::null,
        const bool spatial = true)
    {
        const result<entity> created = world.create_entity(
            { .id = id,
              .name = std::format("entity {}", id),
              .category = category,
              .parent = parent,
              .spatial = spatial });
        REQUIRE(created);
        return *created;
    }

    scene::mesh_handle make_mesh(scene::asset_cache& assets)
    {
        scene::mesh_builder builder{ scene::mesh_topology::triangles };
        for (int vertex = 0; vertex < 3; ++vertex)
        {
            builder.add_vertex({ static_cast<float>(vertex), 0.0f, 0.0f });
        }
        result<scene::mesh_data> built = std::move(builder).build();
        REQUIRE(built);
        const result<scene::mesh_handle> mesh = assets.create_mesh(std::move(*built));
        REQUIRE(mesh);
        return *mesh;
    }

    scene::material_handle make_material(scene::asset_cache& assets)
    {
        const result<scene::material_handle> material = assets.create_material({});
        REQUIRE(material);
        return *material;
    }
} // namespace

TEST_CASE("destroy_entity destroys and reports the whole subtree", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const entity vehicle = create(world, 100, entity_category::vehicle);
    std::vector<std::pair<entity, scene::object_id>> subtree{ { vehicle, 100 } };
    for (scene::object_id id = 101; id <= 104; ++id)
    {
        subtree.emplace_back(create(world, id, entity_category::sensor, vehicle), id);
    }
    const entity helper = create(world, 0, entity_category::helper, vehicle);
    const scene::object_id helper_id = world.registry().get<identity>(helper).id;
    subtree.emplace_back(helper, helper_id);
    const entity other = create(world, 200, entity_category::object);

    const std::vector<scene::destroyed_entity> destroyed = world.destroy_entity(vehicle);
    REQUIRE(destroyed.size() == 6);
    CHECK(destroyed.back().handle == vehicle); // children first
    CHECK(destroyed.back().id == 100);

    std::set<std::pair<entity, scene::object_id>> reported;
    for (const scene::destroyed_entity& entry : destroyed)
    {
        reported.emplace(entry.handle, entry.id);
    }
    CHECK(reported == std::set(subtree.begin(), subtree.end()));

    for (const auto& [handle, id] : subtree)
    {
        CHECK_FALSE(world.alive(handle));
        CHECK_FALSE(world.find(id).has_value());
    }
    CHECK(world.alive(other));
    CHECK(world.find(200) == other);

    SECTION("a non-spatial entity destroys only itself")
    {
        const entity settings = create(world, 300, entity_category::group, entt::null, false);
        CHECK_FALSE(world.registry().all_of<node_reference>(settings));
        const std::vector<scene::destroyed_entity> alone = world.destroy_entity(settings);
        REQUIRE(alone.size() == 1);
        CHECK(alone[0].id == 300);
        CHECK(world.alive(other));
    }
}

TEST_CASE("create_entity checks ids and parents, and find resolves both kinds of id", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const entity car = create(world, 1042, entity_category::object);
    const entity helper = create(world, 0, entity_category::helper);
    const scene::object_id helper_id = world.registry().get<identity>(helper).id;
    CHECK(scene::is_helper(helper_id));
    CHECK(world.find(1042) == car);
    CHECK(world.find(helper_id) == helper);
    CHECK_FALSE(world.find(7).has_value());

    // Spatial entities get a node, but no previous_world before the tick ends
    CHECK(world.registry().all_of<node_reference>(car));
    CHECK_FALSE(world.registry().all_of<previous_world>(car));
    CHECK(world.registry().get<identity>(car).name == "entity 1042");
    CHECK(world.registry().get<identity>(car).category == entity_category::object);

    const auto entities_before = world.registry().view<identity>().size();
    const std::uint64_t version_before = world.scene_version();

    SECTION("a duplicate id fails without creating anything")
    {
        const result<entity> duplicate = world.create_entity({ .id = 1042, .name = "again" });
        REQUIRE_FALSE(duplicate);
        CHECK(scene::is_error(duplicate.error(), scene::error_kind::duplicate_id));
    }

    SECTION("a dead parent fails")
    {
        const entity parent = create(world, 5, entity_category::group);
        world.destroy_entity(parent);
        const auto entities = world.registry().view<identity>().size();
        const result<entity> orphan = world.create_entity({ .id = 6, .parent = parent });
        REQUIRE_FALSE(orphan);
        CHECK(scene::is_error(orphan.error(), scene::error_kind::invalid_parent));
        CHECK(world.registry().view<identity>().size() == entities);
        CHECK_FALSE(world.find(6).has_value());
        return;
    }

    SECTION("a non-spatial parent fails, and so does a parent for a non-spatial entity")
    {
        const entity settings = create(world, 300, entity_category::group, entt::null, false);
        const result<entity> under_settings = world.create_entity({ .id = 301, .parent = settings });
        REQUIRE_FALSE(under_settings);
        CHECK(scene::is_error(under_settings.error(), scene::error_kind::invalid_parent));

        const result<entity> non_spatial_child = world.create_entity({ .id = 302, .parent = car, .spatial = false });
        REQUIRE_FALSE(non_spatial_child);
        CHECK(scene::is_error(non_spatial_child.error(), scene::error_kind::invalid_parent));
        return;
    }

    CHECK(world.registry().view<identity>().size() == entities_before);
    CHECK(world.scene_version() == version_before);
}

TEST_CASE("helper ids skip ids in use and restart after clear", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const scene::object_id first = world.allocate_helper_id();
    CHECK(scene::is_helper(first));
    create(world, first + 1, entity_category::helper); // an explicit id in the helper range
    const scene::object_id next = world.allocate_helper_id();
    CHECK(next == first + 2);

    world.clear();
    CHECK(world.allocate_helper_id() == first);
}

TEST_CASE("selection_target follows represents to the owner", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const entity vehicle = create(world, 100, entity_category::vehicle);
    const entity label_helper = create(world, 0, entity_category::helper, vehicle);
    world.registry().emplace<represents>(label_helper, vehicle);
    CHECK(world.selection_target(label_helper) == vehicle);
    CHECK(world.selection_target(vehicle) == vehicle);

    // Through a chain of helpers
    const entity edge_helper = create(world, 0, entity_category::helper, vehicle);
    world.registry().emplace<represents>(edge_helper, label_helper);
    CHECK(world.selection_target(edge_helper) == vehicle);

    SECTION("a cycle is a debug check")
    {
        tests::recorded_assertions assertions;
        tests::log_capture errors{ "scene" };
        world.registry().emplace<represents>(vehicle, edge_helper);
        CHECK(world.selection_target(edge_helper) == edge_helper);
#ifndef NDEBUG
        CHECK(assertions.contains("selection_target: `represents` forms a cycle"));
#endif
    }
}

TEST_CASE("mesh and material components retain and release their assets", "[scene][unit]")
{
    scene::asset_cache assets;
    const scene::asset_statistics start = assets.statistics();
    {
        scene::scene world{ assets };
        const entity car = create(world, 1, entity_category::object);

        const scene::mesh_handle first_mesh = make_mesh(assets);
        const scene::material_handle first_material = make_material(assets);
        world.registry().emplace<mesh>(car, mesh{ .asset = first_mesh });
        world.registry().emplace<material>(car, material{ .materials = { first_material, first_material } });

        // The components hold their own references: the test's release leaves the assets alive
        assets.release(first_mesh);
        assets.release(first_material);
        CHECK(assets.statistics().meshes == start.meshes + 1);
        CHECK(assets.statistics().materials == start.materials + 1);
        CHECK(assets.current(first_mesh).data != nullptr);

        SECTION("replace releases the old handles and retains the new ones")
        {
            const scene::mesh_handle second_mesh = make_mesh(assets);
            const scene::material_handle second_material = make_material(assets);
            world.registry().replace<mesh>(car, mesh{ .asset = second_mesh });
            world.registry().patch<material>(car, [&](material& value) { value.materials = { second_material }; });
            assets.release(second_mesh);
            assets.release(second_material);

            // The first ones are freed, the second ones live on through the components
            CHECK(assets.statistics().meshes == start.meshes + 1);
            CHECK(assets.statistics().materials == start.materials + 1);
            CHECK(assets.current(second_mesh).data != nullptr);
            CHECK(assets.current(second_material).data != nullptr);

            // Replacing with the same handle keeps it
            world.registry().replace<mesh>(car, mesh{ .asset = second_mesh });
            CHECK(assets.statistics().meshes == start.meshes + 1);

            world.registry().remove<mesh>(car);
            world.registry().remove<material>(car);
            CHECK(assets.statistics().meshes == start.meshes);
            CHECK(assets.statistics().materials == start.materials);
        }

        SECTION("destroy_entity releases them")
        {
            world.destroy_entity(car);
            CHECK(assets.statistics().meshes == start.meshes);
            CHECK(assets.statistics().materials == start.materials);
        }

        SECTION("clear releases them, but not what the test holds")
        {
            const scene::mesh_handle held = make_mesh(assets);
            const entity other = create(world, 2, entity_category::object);
            world.registry().emplace<mesh>(other, mesh{ .asset = held });

            world.clear();
            CHECK(assets.statistics().meshes == start.meshes + 1); // the test's reference to `held`
            CHECK(assets.statistics().materials == start.materials);
            assets.release(held);
            CHECK(assets.statistics().meshes == start.meshes);
        }

        SECTION("the scene's destructor releases them") {}
    }
    CHECK(assets.statistics().meshes == start.meshes);
    CHECK(assets.statistics().materials == start.materials);
}

TEST_CASE("components scene owns are written only through scene", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const entity car = create(world, 1, entity_category::object);

    tests::recorded_assertions assertions;
    tests::log_capture errors{ "scene" };

    SECTION("emplace<identity>")
    {
        const entity outsider = world.registry().create();
        world.registry().emplace<identity>(outsider, identity{ .id = 99 });
#ifndef NDEBUG
        CHECK(assertions.contains("identity is owned by scene"));
#endif
    }

    SECTION("patch<node_reference>")
    {
        world.registry().patch<node_reference>(car, [](node_reference&) {});
#ifndef NDEBUG
        CHECK(assertions.contains("node_reference is owned by scene"));
#endif
    }

    SECTION("emplace<previous_world>")
    {
        world.registry().emplace<previous_world>(car);
#ifndef NDEBUG
        CHECK(assertions.contains("previous_world is owned by scene"));
#endif
    }

    SECTION("scene's own writes don't assert")
    {
        const entity child = create(world, 2, entity_category::object, car);
        world.destroy_entity(car);
        world.clear();
        CHECK_FALSE(world.alive(child));
        CHECK(assertions.count() == 0);
    }
}

TEST_CASE("tree changes report creates, destroys and helpers", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const std::uint64_t start_version = world.scene_version();
    const entity parent = create(world, 10, entity_category::vehicle);
    create(world, 11, entity_category::sensor, parent);
    world.destroy_entity(parent);
    CHECK(world.scene_version() == start_version + 3);

    std::vector<scene::tree_change> changes = world.take_tree_changes();
    REQUIRE(changes.size() == 3);
    CHECK(changes[0].kind == tree_change_kind::added);
    CHECK(changes[0].id == 10);
    CHECK(changes[0].parent == 0);
    CHECK(changes[0].name == "entity 10");
    CHECK(changes[0].category == entity_category::vehicle);
    CHECK(changes[1].kind == tree_change_kind::added);
    CHECK(changes[1].id == 11);
    CHECK(changes[1].parent == 10);
    CHECK(changes[2].kind == tree_change_kind::removed); // one for the root
    CHECK(changes[2].id == 10);
    CHECK(world.take_tree_changes().empty());

    SECTION("a helper with represents is hidden")
    {
        const entity vehicle = create(world, 20, entity_category::vehicle);
        const entity label_helper = create(world, 0, entity_category::helper, vehicle);
        world.registry().emplace<represents>(label_helper, vehicle);

        changes = world.take_tree_changes();
        REQUIRE(changes.size() == 2);
        CHECK_FALSE(changes[0].hidden);
        CHECK(changes[1].kind == tree_change_kind::added);
        CHECK(changes[1].hidden); // emplaced before take_tree_changes: the added change itself

        // Gaining or losing represents after the GUI saw the entity is a renamed change
        const entity late_helper = create(world, 0, entity_category::helper, vehicle);
        world.take_tree_changes();
        world.registry().emplace<represents>(late_helper, vehicle);
        world.registry().remove<represents>(label_helper);
        changes = world.take_tree_changes();
        REQUIRE(changes.size() == 2);
        CHECK(changes[0].kind == tree_change_kind::renamed);
        CHECK(changes[0].hidden);
        CHECK(changes[1].kind == tree_change_kind::renamed);
        CHECK_FALSE(changes[1].hidden);

        // Destroying a hidden helper records nothing extra for its represents
        world.destroy_entity(late_helper);
        changes = world.take_tree_changes();
        REQUIRE(changes.size() == 1);
        CHECK(changes[0].kind == tree_change_kind::removed);
        CHECK(changes[0].hidden);
    }

    SECTION("clear records one removed change per root")
    {
        const entity root = create(world, 30, entity_category::vehicle);
        create(world, 31, entity_category::sensor, root);
        create(world, 32, entity_category::object);
        create(world, 33, entity_category::group, entt::null, false);
        world.take_tree_changes();

        world.clear();
        changes = world.take_tree_changes();
        std::set<scene::object_id> removed;
        for (const scene::tree_change& change : changes)
        {
            CHECK(change.kind == tree_change_kind::removed);
            removed.insert(change.id);
        }
        CHECK(removed == std::set<scene::object_id>{ 30, 32, 33 });
        CHECK(world.registry().view<identity>().size() == 0);
        CHECK_FALSE(world.find(31).has_value());
    }
}

TEST_CASE("a call from another thread asserts", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const entity car = create(world, 1, entity_category::object);

    tests::recorded_assertions assertions;
    tests::log_capture errors{ "scene" };
    std::thread other{ [&] { [[maybe_unused]] const bool alive = world.alive(car); } };
    other.join();
#ifndef NDEBUG
    CHECK(assertions.contains("alive: called from a thread other than the simulation thread"));
#else
    CHECK(assertions.count() == 0);
#endif
}
