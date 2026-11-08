// The scene representation's belief geometry and lidar sweeps (docs/tasks/12-visualizer.md §12.9): one mesh per chunk
// with its colours, new versions for changed chunks only, released meshes for removed ones, progressive points with a
// reproducible shuffle, and lidar sweeps at their own poses, recoloured by the colour mode.

#include "helpers/recorded_assertions.h"
#include "representation/dynamic_mesh.h"
#include "representation/geometry_representation.h"
#include "representation/lidar_representation.h"
#include "representation/presentation_state.h"
#include "representation/representation_system.h"

#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/scene.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using avis::result;
namespace representation = avis::visualizer::representation;
namespace scene = avis::scene;
namespace tests = avis::tests;
using namespace avis::scene::components;
using namespace std::chrono_literals;

namespace
{
    constexpr scene::object_id driver_id(const std::uint64_t index) noexcept
    {
        return (std::uint64_t{ 1 } << 56) | index;
    }

    struct geometry_fixture
    {
        geometry_fixture()
        {
            result<std::unique_ptr<representation::representation_system>> created =
                representation::representation_system::create(assets);
            REQUIRE(created);
            system = std::move(*created);
            state.grid = false;
            state.axes_gizmo = false;
        }

        void update()
        {
            world.update_world_transforms();
            system->update(world, state);
            world.update_world_transforms();
        }

        scene::entity
            add(const std::uint64_t index, const scene::entity_category category = scene::entity_category::geometry)
        {
            const result<scene::entity> created = world.create_entity(
                { .id = driver_id(index), .name = "entity " + std::to_string(index), .category = category });
            REQUIRE(created);
            world.registry().emplace<driver_entity>(*created, driver_entity{ .category = category });
            return *created;
        }

        scene::versioned_mesh mesh_of(const scene::entity helper) const
        {
            REQUIRE(world.registry().all_of<mesh>(helper));
            return assets.current(world.registry().get<mesh>(helper).asset);
        }

        const scene::material_description& material_of(const scene::entity helper) const
        {
            const scene::versioned_material current =
                assets.current(world.registry().get<material>(helper).materials.at(0));
            REQUIRE(current.data);
            return *current.data;
        }

        scene::asset_cache assets;
        scene::scene world{ assets };
        std::unique_ptr<representation::representation_system> system;
        representation::presentation_state state = representation::presentation_state::from_settings({});
    };

    geometry_chunk triangle_chunk(const std::uint32_t id)
    {
        geometry_chunk chunk;
        chunk.id = id;
        chunk.topology = geometry_topology::triangles;
        chunk.origin = { 10.0, -2.0, 0.5 };
        chunk.positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
        chunk.colors = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } };
        chunk.style.opacity = 0.5f;
        return chunk;
    }

    geometry_chunk point_chunk(const std::uint32_t id, const std::uint32_t count)
    {
        geometry_chunk chunk;
        chunk.id = id;
        chunk.topology = geometry_topology::points;
        chunk.style.point_size = 0.1f;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            chunk.positions.emplace_back(static_cast<float>(index), 0.0f, 0.0f);
        }
        return chunk;
    }

    lidar_sweep
        make_sweep(const std::chrono::milliseconds time, const Eigen::Vector3d& position, const std::uint32_t count)
    {
        lidar_sweep sweep;
        sweep.capture_time = time;
        sweep.sensor_position = position;
        sweep.sensor_orientation = Eigen::Quaternionf{ Eigen::AngleAxisf{ 0.5f, Eigen::Vector3f::UnitZ() } };
        for (std::uint32_t index = 0; index < count; ++index)
        {
            sweep.positions.emplace_back(static_cast<float>(index % 50), static_cast<float>(index / 50), -1.0f);
            sweep.intensity.push_back(static_cast<std::uint8_t>(index % 256));
            sweep.ring.push_back(static_cast<std::uint8_t>(index % 16));
        }
        return sweep;
    }
} // namespace

TEST_CASE("representation: a triangle chunk becomes a mesh with its colours", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    geometry_fixture fixture;
    const std::uint32_t meshes_before = fixture.assets.statistics().meshes;
    const scene::entity area = fixture.add(1);
    fixture.world.registry().emplace<belief_geometry>(area).replace(triangle_chunk(7));
    fixture.update();

    const scene::entity helper = fixture.system->geometry().chunk_helper(area, 7);
    REQUIRE(helper != entt::null);
    entt::registry& registry = fixture.world.registry();
    REQUIRE(registry.all_of<represents, material, render_options>(helper));
    CHECK(registry.get<represents>(helper).owner == area);
    CHECK(registry.get<identity>(helper).name == "entity 1 · chunk 7");
    CHECK(registry.get<render_options>(helper).layer == scene::draw_layer::world);
    CHECK(fixture.world.local(helper).position.isApprox(Eigen::Vector3f{ 10.0f, -2.0f, 0.5f }));

    const scene::versioned_mesh first = fixture.mesh_of(helper);
    REQUIRE(first.data);
    CHECK(first.version == 1);
    CHECK(first.data->topology == scene::mesh_topology::triangles);
    CHECK_FALSE(first.data->progressive);
    CHECK(first.data->positions.size() == 3);
    CHECK(
        first.data->color0 ==
        std::vector<std::array<std::uint8_t, 4>>{ { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } });
    CHECK(first.data->normals.size() == 3); // lit, so smooth normals were made

    const scene::material_description& look = fixture.material_of(helper);
    CHECK(look.use_vertex_color);
    CHECK(look.shading == scene::shading_model::lit);
    CHECK(look.alpha == scene::alpha_mode::blend);
    CHECK(look.base_color_factor.alpha == 0.5f);

    SECTION("a second update of the chunk bumps the version of the same mesh")
    {
        const scene::mesh_handle handle = registry.get<mesh>(helper).asset;
        registry.patch<belief_geometry>(
            area,
            [](belief_geometry& geometry)
            {
                geometry_chunk chunk = triangle_chunk(7);
                chunk.positions[2].z() = 2.0f;
                geometry.replace(std::move(chunk));
            });
        fixture.update();
        CHECK(fixture.system->geometry().chunk_helper(area, 7) == helper);
        CHECK(registry.get<mesh>(helper).asset == handle);
        CHECK(fixture.mesh_of(helper).version == 2);

        // The same content again: no new version
        registry.patch<belief_geometry>(
            area,
            [](belief_geometry& geometry)
            {
                geometry_chunk chunk = triangle_chunk(7);
                chunk.positions[2].z() = 2.0f;
                geometry.replace(std::move(chunk));
            });
        fixture.update();
        CHECK(fixture.mesh_of(helper).version == 2);
    }

    SECTION("another chunk changing leaves this one's version alone")
    {
        registry.patch<belief_geometry>(area, [](belief_geometry& geometry) { geometry.replace(triangle_chunk(8)); });
        fixture.update();
        CHECK(fixture.mesh_of(helper).version == 1);
        CHECK(fixture.system->geometry().chunk_helper(area, 8) != entt::null);
    }

    SECTION("removing the chunk releases its mesh")
    {
        registry.patch<belief_geometry>(area, [](belief_geometry& geometry) { geometry.remove(7); });
        fixture.update();
        CHECK_FALSE(fixture.world.alive(helper));
        CHECK(fixture.system->geometry().chunk_helper(area, 7) == entt::null);
        CHECK(fixture.assets.statistics().meshes == meshes_before);
    }

    SECTION("removing the component releases every mesh")
    {
        registry.remove<belief_geometry>(area);
        fixture.update();
        CHECK_FALSE(fixture.world.alive(helper));
        CHECK(fixture.assets.statistics().meshes == meshes_before);
    }

    SECTION("destroying the entity takes its chunks along")
    {
        static_cast<void>(fixture.world.destroy_entity(area));
        fixture.update();
        CHECK_FALSE(fixture.world.alive(helper));
        CHECK(fixture.assets.statistics().meshes == meshes_before);
    }
    CHECK(assertions.count() == 0);
}

TEST_CASE("representation: line strips, overlay chunks and broken chunks", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    geometry_fixture fixture;
    const scene::entity path = fixture.add(2);
    geometry_chunk strip;
    strip.id = 1;
    strip.topology = geometry_topology::line_strip;
    strip.positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 2.0f, 1.0f, 0.0f }, { 3.0f, 1.0f, 0.0f } };
    strip.style.lit = false;
    strip.style.line_width = 3.0f;
    strip.style.layer = scene::draw_layer::overlay;
    strip.style.style_color = scene::color::from_srgb8({ 255, 200, 0, 255 });

    geometry_chunk broken = triangle_chunk(2);
    broken.colors.pop_back(); // two colours for three positions

    belief_geometry& geometry = fixture.world.registry().emplace<belief_geometry>(path);
    geometry.replace(strip);
    geometry.replace(broken);
    fixture.update();

    const scene::entity helper = fixture.system->geometry().chunk_helper(path, 1);
    REQUIRE(helper != entt::null);
    const scene::versioned_mesh lines = fixture.mesh_of(helper);
    REQUIRE(lines.data);
    CHECK(lines.data->topology == scene::mesh_topology::lines);
    CHECK(lines.data->indices == std::vector<std::uint32_t>{ 0, 1, 1, 2, 2, 3 });
    CHECK(lines.data->normals.empty());
    CHECK(fixture.world.registry().get<render_options>(helper).layer == scene::draw_layer::overlay);
    const scene::material_description& look = fixture.material_of(helper);
    CHECK(look.shading == scene::shading_model::unlit);
    CHECK(look.line_width == 3.0f);
    CHECK_FALSE(look.use_vertex_color);
    CHECK(look.alpha == scene::alpha_mode::opaque);

    // The broken chunk has no mesh, and the budget doesn't count it
    CHECK(fixture.system->geometry().chunk_helper(path, 2) == entt::null);
    CHECK(fixture.system->reduction().skipped == 0);
    CHECK(assertions.count() == 0);
}

TEST_CASE("representation: a points chunk is progressive, with a reproducible shuffle", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    const geometry_chunk chunk = point_chunk(3, 1000);

    const result<scene::mesh_data> first = representation::build_chunk_mesh(chunk);
    const result<scene::mesh_data> second = representation::build_chunk_mesh(chunk);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->topology == scene::mesh_topology::points);
    CHECK(first->progressive);
    CHECK(first->indices.empty());
    CHECK(first->positions == second->positions); // the same order on both runs

    // A permutation of the chunk's points, not their order
    CHECK(first->positions != chunk.positions);
    std::vector<float> sorted;
    for (const Eigen::Vector3f& position : first->positions)
    {
        sorted.push_back(position.x());
    }
    std::ranges::sort(sorted);
    for (std::size_t index = 0; index < sorted.size(); ++index)
    {
        CHECK(sorted[index] == static_cast<float>(index));
    }

    // Through the representation: two scenes give the same mesh
    std::vector<std::vector<Eigen::Vector3f>> runs;
    for (int run = 0; run < 2; ++run)
    {
        geometry_fixture fixture;
        const scene::entity map = fixture.add(3);
        fixture.world.registry().emplace<belief_geometry>(map).replace(chunk);
        fixture.update();
        const scene::entity helper = fixture.system->geometry().chunk_helper(map, 3);
        REQUIRE(helper != entt::null);
        const scene::versioned_mesh points = fixture.mesh_of(helper);
        REQUIRE(points.data);
        CHECK(points.data->progressive);
        CHECK(fixture.material_of(helper).point_size == 0.1f);
        runs.push_back(points.data->positions);
    }
    CHECK(runs[0] == runs[1]);
    CHECK(assertions.count() == 0);
}

TEST_CASE("representation: lidar sweeps at their own poses, recoloured by the mode", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    geometry_fixture fixture;
    const scene::entity velodyne = fixture.add(4, scene::entity_category::sensor);
    entt::registry& registry = fixture.world.registry();
    registry.emplace<lidar_points>(velodyne, lidar_points{ .retention = 3 });
    for (int sweep = 0; sweep < 5; ++sweep)
    {
        registry.patch<lidar_points>(
            velodyne,
            [&](lidar_points& points) { points.push(make_sweep(sweep * 100ms, { 2.0 * sweep, 1.0, 1.7 }, 600)); });
        fixture.update();
    }

    // The last three, oldest first, each at the pose it was captured at
    const representation::lidar_representation& lidar = fixture.system->lidar();
    REQUIRE(lidar.sweep_count(velodyne) == 3);
    std::vector<scene::entity> helpers;
    for (std::size_t index = 0; index < 3; ++index)
    {
        const representation::dynamic_mesh* sweep = lidar.sweep_mesh(velodyne, index);
        REQUIRE(sweep != nullptr);
        REQUIRE(fixture.world.alive(sweep->helper));
        helpers.push_back(sweep->helper);
        CHECK(registry.get<represents>(sweep->helper).owner == velodyne);
        const scene::transform local = fixture.world.local(sweep->helper);
        CHECK(local.position.isApprox(Eigen::Vector3f(2.0f * static_cast<float>(index + 2), 1.0f, 1.7f)));
        CHECK(local.rotation.isApprox(Eigen::Quaternionf{ Eigen::AngleAxisf{ 0.5f, Eigen::Vector3f::UnitZ() } }));
        const scene::versioned_mesh points = fixture.mesh_of(sweep->helper);
        REQUIRE(points.data);
        CHECK(points.data->progressive);
        CHECK(points.data->positions.size() == 600);
        CHECK(points.data->color0.size() == 600); // intensity, the default
        CHECK(points.version == 1);
        CHECK(fixture.material_of(sweep->helper).use_vertex_color);
        CHECK(registry.get<render_options>(sweep->helper).flags.count(scene::instance_flags::casts_shadows) == 0);
    }
    CHECK(registry.get<identity>(helpers[0]).name == "entity 4 · sweep 0.200 s");

    SECTION("a colour-mode change creates new versions of the same meshes")
    {
        std::vector<scene::mesh_handle> handles;
        for (const scene::entity helper : helpers)
        {
            handles.push_back(registry.get<mesh>(helper).asset);
        }
        fixture.state.lidar.color_mode = representation::lidar_color_mode::ring;
        fixture.update();
        for (std::size_t index = 0; index < helpers.size(); ++index)
        {
            CHECK(registry.get<mesh>(helpers[index]).asset == handles[index]);
            CHECK(fixture.mesh_of(helpers[index]).version == 2);
        }

        fixture.state.lidar.color_mode = representation::lidar_color_mode::uniform;
        fixture.update();
        for (const scene::entity helper : helpers)
        {
            const scene::versioned_mesh points = fixture.mesh_of(helper);
            CHECK(points.version == 3);
            CHECK(points.data->color0.empty());
            CHECK_FALSE(fixture.material_of(helper).use_vertex_color);
        }

        // The point size is only the material's
        fixture.state.lidar.point_size = 0.2f;
        fixture.update();
        for (const scene::entity helper : helpers)
        {
            CHECK(fixture.mesh_of(helper).version == 3);
            CHECK(fixture.material_of(helper).point_size == 0.2f);
        }
    }

    SECTION("maximum_points_per_sweep keeps a random prefix")
    {
        fixture.state.representation.lidar.maximum_points_per_sweep = 100;
        fixture.update();
        for (const scene::entity helper : helpers)
        {
            const scene::versioned_mesh points = fixture.mesh_of(helper);
            CHECK(points.data->positions.size() == 100);
            CHECK(points.version == 2);
        }
    }

    SECTION("a sweep without the mode's channel is uniform")
    {
        lidar_sweep plain = make_sweep(500ms, { 0.0, 0.0, 1.7 }, 10);
        plain.intensity.clear();
        registry.patch<lidar_points>(velodyne, [&](lidar_points& points) { points.push(std::move(plain)); });
        fixture.update();
        const representation::dynamic_mesh* newest = lidar.sweep_mesh(velodyne, 2);
        REQUIRE(newest != nullptr);
        CHECK(fixture.mesh_of(newest->helper).data->color0.empty());
        CHECK_FALSE(fixture.material_of(newest->helper).use_vertex_color);
        CHECK_FALSE(fixture.world.alive(helpers[0])); // pushed out of the retention
    }

    SECTION("destroying the lidar destroys its sweeps, which aren't its children")
    {
        static_cast<void>(fixture.world.destroy_entity(velodyne));
        fixture.update();
        for (const scene::entity helper : helpers)
        {
            CHECK_FALSE(fixture.world.alive(helper));
        }
        CHECK(fixture.system->reduction().used_bytes == 0);
    }
    CHECK(assertions.count() == 0);
}

TEST_CASE("representation: the height colour map follows world z", "[visualizer][unit]")
{
    lidar_sweep sweep;
    sweep.sensor_position = { 0.0, 0.0, 2.0 };
    sweep.positions = { { 0.0f, 0.0f, -3.0f }, { 0.0f, 0.0f, 2.0f } }; // world z −1 and 4: the ends of the map
    const result<scene::mesh_data> data =
        representation::build_sweep_mesh(sweep, representation::lidar_color_mode::height, 10);
    REQUIRE(data);
    REQUIRE(data->color0.size() == 2);
    std::vector<std::array<std::uint8_t, 4>> colors = data->color0;
    std::ranges::sort(colors);
    std::vector<std::array<std::uint8_t, 4>> ends{ representation::lidar_color_map(0.0f),
                                                   representation::lidar_color_map(1.0f) };
    std::ranges::sort(ends);
    CHECK(colors == ends);
}
