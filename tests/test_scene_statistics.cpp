// scene::statistics (docs/tasks/10-scene-management.md §10.12, 08 §8.5)

#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/components/spatial.h"
#include "avis/scene/components/viewing.h"
#include "avis/scene/scene.h"
#include "avis/scene/snapshot.h"
#include "avis/scene/statistics.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <random>
#include <vector>

using namespace avis;
using namespace avis::scene::components;
using namespace std::chrono_literals;
using avis::scene::entity;
using avis::scene::entity_category;

namespace
{
    scene::mesh_handle make_mesh(scene::asset_cache& assets, const float size)
    {
        scene::mesh_builder builder{ scene::mesh_topology::triangles };
        builder.add_vertex({ 0.0f, 0.0f, 0.0f }).add_vertex({ size, 0.0f, 0.0f }).add_vertex({ 0.0f, size, 0.0f });
        result<scene::mesh_data> built = std::move(builder).build();
        REQUIRE(built);
        const result<scene::mesh_handle> mesh = assets.create_mesh(std::move(*built));
        REQUIRE(mesh);
        return *mesh;
    }

    void check_against_registry(scene::scene& world)
    {
        const scene::scene_statistics statistics = world.statistics();
        auto& registry = world.registry();

        CHECK(statistics.entities == registry.view<identity>().size());
        CHECK(statistics.nodes == registry.view<node_reference>().size());
        CHECK(statistics.labels == registry.view<label>().size());
        CHECK(statistics.trails == registry.view<trail>().size());

        std::array<std::uint32_t, scene::entity_category_count> per_category{};
        for (const auto [handle, who] : registry.view<identity>().each())
        {
            ++per_category[static_cast<std::size_t>(who.category)];
        }
        CHECK(statistics.entities_per_category == per_category);

        std::size_t samples = 0;
        for (const auto [handle, path] : registry.view<trail>().each())
        {
            samples += path.samples.size();
        }
        CHECK(statistics.trail_samples == samples);
    }
} // namespace

TEST_CASE("statistics match a registry scan after random creates and destroys", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    std::mt19937 random{ 1012 };
    std::vector<entity> alive;
    std::int64_t time = 0;

    for (int operation = 0; operation < 10'000; ++operation)
    {
        const bool destroy = !alive.empty() && random() % 3 == 0;
        if (destroy)
        {
            const entity victim = alive[random() % alive.size()];
            for (const scene::destroyed_entity& destroyed : world.destroy_entity(victim))
            {
                std::erase(alive, destroyed.handle);
            }
        }
        else
        {
            const auto category = static_cast<entity_category>(random() % scene::entity_category_count);
            const bool spatial = random() % 8 != 0;
            entity parent = entt::null;
            if (spatial && !alive.empty() && random() % 2 == 0)
            {
                const entity candidate = alive[random() % alive.size()];
                if (world.registry().all_of<node_reference>(candidate))
                {
                    parent = candidate;
                }
            }
            scene::entity_description description{ .category = category, .parent = parent, .spatial = spatial };
            description.local.position = Eigen::Vector3f{ static_cast<float>(operation % 50), 0.0f, 0.0f };
            const result<entity> created = world.create_entity(description);
            REQUIRE(created);
            alive.push_back(*created);

            if (spatial && random() % 4 == 0)
            {
                world.registry().emplace<trail>(*created, trail{ .settings = { .minimum_spacing = 0.0f } });
            }
            if (random() % 5 == 0)
            {
                world.registry().emplace<label>(*created);
            }
        }

        if (operation % 100 == 99)
        {
            // A tick, so the trails gain samples
            world.update_world_transforms();
            world.update_trails(std::chrono::milliseconds{ time += 100 });
            world.end_tick();
        }
    }

    REQUIRE(world.statistics().trail_samples > 0);
    check_against_registry(world);

    world.clear();
    check_against_registry(world);
    CHECK(world.statistics().entities == 0);
    CHECK(world.statistics().nodes == 0);
}

TEST_CASE("geometry bytes return to their start once meshes and snapshot slots are released", "[scene][unit]")
{
    scene::asset_cache assets;
    const scene::scene_statistics start = [&]
    {
        scene::scene probe{ assets };
        return probe.statistics();
    }();

    scene::scene world{ assets };
    scene::snapshot_buffer buffer;
    const auto publish = [&](const simulation::step_index step)
    {
        world.update_world_transforms();
        scene::snapshot& target = buffer.begin_write();
        target.step = step;
        world.make_snapshot(target);
        buffer.publish();
        world.end_tick();
    };

    std::vector<scene::mesh_handle> meshes;
    for (int index = 0; index < 4; ++index)
    {
        meshes.push_back(make_mesh(assets, static_cast<float>(index + 1)));
        const result<entity> created = world.create_entity({ .id = static_cast<scene::object_id>(index + 1) });
        REQUIRE(created);
        world.registry().emplace<mesh>(*created, mesh{ .asset = meshes.back() });
        world.registry().emplace<material>(*created);
    }
    publish(1);

    scene::scene_statistics filled = world.statistics();
    CHECK(filled.step == 1);
    CHECK(filled.meshes == start.meshes + 4);
    CHECK(filled.instances == 4);
    CHECK(filled.current_geometry_bytes > start.current_geometry_bytes);

    // Replace one mesh's data: the old version is retained by the snapshot slot
    scene::mesh_builder builder{ scene::mesh_topology::triangles };
    builder.add_vertex({ 0.0f, 0.0f, 0.0f }).add_vertex({ 9.0f, 0.0f, 0.0f }).add_vertex({ 0.0f, 9.0f, 0.0f });
    REQUIRE(assets.update_mesh(meshes[0], *std::move(builder).build()));
    CHECK(world.statistics().retained_geometry_bytes > start.retained_geometry_bytes);

    // Every mesh released: the components go, then the test's own references
    world.clear();
    for (const scene::mesh_handle mesh : meshes)
    {
        assets.release(mesh);
    }
    CHECK(world.statistics().meshes == start.meshes);
    CHECK(world.statistics().current_geometry_bytes == start.current_geometry_bytes);
    CHECK(world.statistics().retained_geometry_bytes > start.retained_geometry_bytes); // the front slot holds them

    // Both slots reused
    publish(2);
    publish(3);
    const scene::scene_statistics end = world.statistics();
    CHECK(end.step == 3);
    CHECK(end.instances == 0);
    CHECK(end.current_geometry_bytes == start.current_geometry_bytes);
    CHECK(end.retained_geometry_bytes == start.retained_geometry_bytes);
}

TEST_CASE("the timings are non-zero after a tick", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const scene::mesh_handle body = make_mesh(assets, 1.0f);

    for (scene::object_id id = 1; id <= 1000; ++id)
    {
        const result<entity> created = world.create_entity({ .id = id });
        REQUIRE(created);
        world.registry().emplace<mesh>(*created, mesh{ .asset = body });
        world.registry().emplace<material>(*created);
    }
    CHECK(world.statistics().transform_update_milliseconds == 0.0);
    CHECK(world.statistics().snapshot_build_milliseconds == 0.0);

    scene::snapshot built;
    world.update_world_transforms();
    world.make_snapshot(built);
    world.end_tick();

    const scene::scene_statistics statistics = world.statistics();
    CHECK(statistics.transform_update_milliseconds > 0.0);
    CHECK(statistics.snapshot_build_milliseconds > 0.0);
    CHECK(statistics.instances == 1000);
    CHECK(statistics.entities == 1000);
    CHECK(statistics.nodes == 1000);
    CHECK(statistics.entities_per_category[static_cast<std::size_t>(entity_category::helper)] == 1000);

    world.clear();
    assets.release(body);
}
