// scene::make_snapshot: instance order, asset tables, materials per primitive, world_previous, asset versions held by
// snapshots and the steady state's allocations (docs/tasks/10-scene-management.md §10.11)

#include "helpers/allocation_failure.h"

#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/components/spatial.h"
#include "avis/scene/scene.h"
#include "avis/scene/snapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <new>
#include <random>
#include <set>
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
        const entity_category category = entity_category::object,
        const entity parent = entt::null,
        const transform& local = {})
    {
        const result<entity> created = world.create_entity(
            { .id = id, .name = std::format("entity {}", id), .category = category, .parent = parent, .local = local });
        REQUIRE(created);
        return *created;
    }

    transform at(const Eigen::Vector3f& position)
    {
        transform made;
        made.position = position;
        return made;
    }

    // A triangle per primitive, `scale` metres wide, so different meshes have different byte sizes
    scene::mesh_data make_mesh_data(const int primitives = 1, const float scale = 1.0f)
    {
        scene::mesh_builder builder{ scene::mesh_topology::triangles };
        for (int primitive = 0; primitive < primitives; ++primitive)
        {
            for (int vertex = 0; vertex < 3; ++vertex)
            {
                builder.add_vertex({ scale * static_cast<float>(vertex), static_cast<float>(primitive), 0.0f });
            }
            builder.end_primitive();
        }
        result<scene::mesh_data> built = std::move(builder).build();
        REQUIRE(built);
        return std::move(*built);
    }

    scene::mesh_handle make_mesh(scene::asset_cache& assets, const int primitives = 1)
    {
        const result<scene::mesh_handle> mesh = assets.create_mesh(make_mesh_data(primitives));
        REQUIRE(mesh);
        return *mesh;
    }

    scene::material_handle make_material(scene::asset_cache& assets, scene::material_description description = {})
    {
        const result<scene::material_handle> material = assets.create_material(std::move(description));
        REQUIRE(material);
        return *material;
    }

    scene::texture_handle make_texture(scene::asset_cache& assets)
    {
        scene::texture_data texture;
        texture.width = 2;
        texture.height = 2;
        texture.mips.emplace_back(16);
        const result<scene::texture_handle> created = assets.create_texture(std::move(texture));
        REQUIRE(created);
        return *created;
    }

    // Gives the entity a mesh and materials; the components hold their own references
    void draw(
        scene::scene& world,
        const entity entity,
        const scene::mesh_handle mesh,
        std::vector<scene::material_handle> materials)
    {
        world.registry().emplace<scene::components::mesh>(entity, scene::components::mesh{ .asset = mesh });
        world.registry().emplace<material>(entity, material{ .materials = std::move(materials) });
    }

    // The tick sequence of 08 §9.6 with one snapshot
    void tick(scene::scene& world, scene::snapshot& target)
    {
        world.update_world_transforms();
        world.make_snapshot(target);
        world.end_tick();
    }

    void check_tables(const scene::snapshot& built)
    {
        CHECK(std::ranges::is_sorted(built.meshes, {}, &scene::versioned_mesh_entry::handle));
        CHECK(std::ranges::adjacent_find(built.meshes, {}, &scene::versioned_mesh_entry::handle) == built.meshes.end());
        CHECK(std::ranges::is_sorted(built.materials, {}, &scene::versioned_material_entry::handle));
        CHECK(
            std::ranges::adjacent_find(built.materials, {}, &scene::versioned_material_entry::handle) ==
            built.materials.end());

        for (const scene::render_instance& instance : built.instances)
        {
            CHECK(std::ranges::binary_search(built.meshes, instance.mesh, {}, &scene::versioned_mesh_entry::handle));
            if (instance.material.valid())
            {
                CHECK(
                    std::ranges::binary_search(
                        built.materials, instance.material, {}, &scene::versioned_material_entry::handle));
            }
        }
        for (const scene::versioned_mesh_entry& entry : built.meshes)
        {
            CHECK(entry.mesh.data != nullptr);
            CHECK(
                std::ranges::any_of(
                    built.instances,
                    [&](const scene::render_instance& instance) { return instance.mesh == entry.handle; }));
        }
    }
} // namespace

TEST_CASE("instances come out sorted by id and list each asset once", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const scene::mesh_handle shared_mesh = make_mesh(assets);
    const scene::material_handle shared_material = make_material(assets);

    std::mt19937 random{ 1011 };
    std::vector<entity> drawn;
    std::set<scene::object_id> expected;

    // 100 instances share one mesh and one material; their ids come in random order
    std::vector<scene::object_id> ids(100);
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        ids[index] = 1000 + index * 7;
    }
    std::ranges::shuffle(ids, random);
    for (const scene::object_id id : ids)
    {
        const entity created = create(world, id);
        draw(world, created, shared_mesh, { shared_material });
        drawn.push_back(created);
        expected.insert(id);
    }

    // A few with their own assets, and some that draw nothing
    std::vector<scene::mesh_handle> own_meshes;
    std::vector<scene::material_handle> own_materials;
    for (scene::object_id id = 1; id <= 5; ++id)
    {
        own_meshes.push_back(make_mesh(assets, 2));
        own_materials.push_back(make_material(assets));
        own_materials.push_back(make_material(assets));
        draw(world, create(world, id * 3), own_meshes.back(), { own_materials.end()[-2], own_materials.back() });
        expected.insert(id * 3);
        create(world, 500 + id); // nothing drawn
    }

    scene::snapshot built;
    const auto check_snapshot = [&]
    {
        tick(world, built);
        CHECK(std::ranges::is_sorted(built.instances, {}, &scene::render_instance::id));
        std::set<scene::object_id> found;
        for (const scene::render_instance& instance : built.instances)
        {
            found.insert(instance.id);
        }
        CHECK(found == expected);
        check_tables(built);
    };

    check_snapshot();
    CHECK(built.meshes.size() == 6);
    CHECK(built.materials.size() == 11);
    CHECK(built.instances.size() == 100 + 5 * 2);

    // Destroy some in random order and create more, also with smaller ids
    std::ranges::shuffle(drawn, random);
    for (std::size_t index = 0; index < 40; ++index)
    {
        expected.erase(world.registry().get<identity>(drawn[index]).id);
        world.destroy_entity(drawn[index]);
    }
    for (scene::object_id id = 2; id < 30; id += 3)
    {
        draw(world, create(world, id), shared_mesh, { shared_material });
        expected.insert(id);
    }
    check_snapshot();

    // Losing the mesh takes an entity out without a structural change
    const entity last = drawn.back();
    world.registry().remove<scene::components::mesh>(last);
    expected.erase(world.registry().get<identity>(last).id);
    check_snapshot();

    world.clear();
    tick(world, built);
    CHECK(built.instances.empty());
    CHECK(built.meshes.empty());
    CHECK(built.materials.empty());

    for (const scene::mesh_handle mesh : own_meshes)
    {
        assets.release(mesh);
    }
    for (const scene::material_handle material : own_materials)
    {
        assets.release(material);
    }
    assets.release(shared_mesh);
    assets.release(shared_material);
}

TEST_CASE("a mesh with more primitives than materials repeats the last material", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const scene::mesh_handle mesh = make_mesh(assets, 3);
    const scene::material_handle first = make_material(assets);
    const scene::material_handle second = make_material(assets);

    const entity car = create(world, 7);
    draw(world, car, mesh, { first, second });

    scene::snapshot built;
    tick(world, built);
    REQUIRE(built.instances.size() == 3);
    for (std::uint32_t index = 0; index < 3; ++index)
    {
        CHECK(built.instances[index].id == 7);
        CHECK(built.instances[index].mesh == mesh);
        CHECK(built.instances[index].submesh == index);
    }
    CHECK(built.instances[0].material == first);
    CHECK(built.instances[1].material == second);
    CHECK(built.instances[2].material == second);

    SECTION("a range of the primitives")
    {
        world.registry().patch<scene::components::mesh>(
            car,
            [](scene::components::mesh& range)
            {
                range.primitive_first = 1;
                range.primitive_count = 5; // clamped to the two that remain
            });
        tick(world, built);
        REQUIRE(built.instances.size() == 2);
        CHECK(built.instances[0].submesh == 1);
        CHECK(built.instances[0].material == first);
        CHECK(built.instances[1].submesh == 2);
        CHECK(built.instances[1].material == second);
    }

    SECTION("no materials: the renderer's default")
    {
        world.registry().replace<material>(car, material{});
        tick(world, built);
        REQUIRE(built.instances.size() == 3);
        CHECK_FALSE(built.instances[2].material.valid());
        CHECK(built.materials.empty());
    }

    assets.release(mesh);
    assets.release(first);
    assets.release(second);
}

TEST_CASE("snapshot instances carry render options, visibility and both worlds", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };
    const scene::mesh_handle mesh = make_mesh(assets);
    const scene::material_handle surface = make_material(assets);

    const entity car = create(world, 1, entity_category::vehicle, entt::null, at({ 1.0f, 0.0f, 0.0f }));
    draw(world, car, mesh, { surface });

    scene::snapshot built;
    tick(world, built);
    REQUIRE(built.instances.size() == 1);
    {
        // Created this tick: no history, so it pops in
        const scene::render_instance& instance = built.instances[0];
        CHECK(instance.flags == (scene::default_instance_flags | scene::instance_flags::history_invalid));
        CHECK(instance.world_previous == instance.world_current);
        CHECK(instance.world_current.block<3, 1>(0, 3).isApprox(Eigen::Vector3f{ 1.0f, 0.0f, 0.0f }));
        CHECK(instance.layer == scene::draw_layer::world);
    }

    // The next tick: world_previous is the previous tick's world
    world.set_local(car, at({ 2.0f, 0.0f, 0.0f }));
    const entity label = create(world, 0, entity_category::helper, car, at({ 0.0f, 0.0f, 3.0f }));
    draw(world, label, mesh, { surface });
    world.registry().emplace<render_options>(
        label,
        render_options{ .layer = scene::draw_layer::overlay_on_top,
                        .layer_priority = 4,
                        .flags = scene::instance_flags::visible | scene::instance_flags::screen_aligned,
                        .maximum_view_distance = 150.0f });
    world.registry().emplace<visibility>(car, visibility{ .visible = false });
    tick(world, built);
    REQUIRE(built.instances.size() == 2);
    {
        const scene::render_instance& instance = built.instances[0];
        CHECK(instance.id == 1);
        CHECK(instance.world_previous.block<3, 1>(0, 3).isApprox(Eigen::Vector3f{ 1.0f, 0.0f, 0.0f }));
        CHECK(instance.world_current.block<3, 1>(0, 3).isApprox(Eigen::Vector3f{ 2.0f, 0.0f, 0.0f }));
        CHECK(instance.flags == (scene::instance_flags::casts_shadows | scene::instance_flags::pickable));
    }
    {
        const scene::render_instance& instance = built.instances[1];
        CHECK(scene::is_helper(instance.id));
        CHECK(instance.layer == scene::draw_layer::overlay_on_top);
        CHECK(instance.layer_priority == 4);
        CHECK(instance.maximum_view_distance == 150.0f);
        CHECK(
            instance.flags == (scene::instance_flags::visible | scene::instance_flags::screen_aligned |
                               scene::instance_flags::history_invalid));
        CHECK(instance.world_current.block<3, 1>(0, 3).isApprox(Eigen::Vector3f{ 2.0f, 0.0f, 3.0f }));
    }

    // A republish after end_tick: nothing moves, and the label has its history now
    world.make_snapshot(built);
    CHECK(built.instances[0].world_previous == built.instances[0].world_current);
    CHECK_FALSE(built.instances[1].flags & scene::instance_flags::history_invalid);

    assets.release(mesh);
    assets.release(surface);
}

TEST_CASE("snapshots carry lights, the environment, textures and the scene version", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const scene::texture_handle shared_texture = make_texture(assets);
    const scene::texture_handle normal_texture = make_texture(assets);
    scene::material_description painted;
    painted.base_color_texture = shared_texture;
    painted.normal_texture = normal_texture;
    scene::material_description decal;
    decal.base_color_texture = shared_texture;
    const scene::material_handle first = make_material(assets, painted);
    const scene::material_handle second = make_material(assets, decal);
    const scene::mesh_handle mesh = make_mesh(assets, 2);
    draw(world, create(world, 3), mesh, { first, second });

    const entity spot = create(world, 9, entity_category::helper, entt::null, at({ 0.0f, 5.0f, 2.0f }));
    world.registry().emplace<light>(
        spot, light{ .kind = scene::light_kind::spot, .intensity = 800.0f, .outer_cone_angle = 0.5f });
    const entity sun = create(world, 4, entity_category::helper);
    world.registry().emplace<light>(sun, light{ .kind = scene::light_kind::directional, .casts_shadows = true });
    create(world, 5, entity_category::helper); // no light: not listed

    scene::environment_settings environment;
    environment.sun_illuminance = 50'000.0f;
    environment.sky_turbidity = 4.0f;
    world.set_environment(environment);

    scene::snapshot built;
    tick(world, built);

    REQUIRE(built.lights.size() == 2);
    CHECK(built.lights[0].id == 4);
    CHECK(built.lights[0].kind == scene::light_kind::directional);
    CHECK(built.lights[0].casts_shadows);
    CHECK(built.lights[1].id == 9);
    CHECK(built.lights[1].kind == scene::light_kind::spot);
    CHECK(built.lights[1].intensity == 800.0f);
    CHECK(built.lights[1].outer_cone_angle == 0.5f);
    CHECK(built.lights[1].world.block<3, 1>(0, 3).isApprox(Eigen::Vector3f{ 0.0f, 5.0f, 2.0f }));

    CHECK(built.environment.sun_illuminance == 50'000.0f);
    CHECK(built.environment.sky_turbidity == 4.0f);
    CHECK(world.environment().sky_turbidity == 4.0f);
    CHECK(built.scene_version == world.scene_version());

    // Two materials, three texture references, two distinct textures
    CHECK(built.materials.size() == 2);
    REQUIRE(built.textures.size() == 2);
    CHECK(built.textures[0].handle < built.textures[1].handle);
    for (const scene::versioned_texture_entry& entry : built.textures)
    {
        CHECK((entry.handle == shared_texture || entry.handle == normal_texture));
        CHECK(entry.texture.data != nullptr);
    }

    assets.release(mesh);
    assets.release(first);
    assets.release(second);
    assets.release(shared_texture);
    assets.release(normal_texture);
}

TEST_CASE("a snapshot keeps the mesh version it holds alive", "[scene][unit]")
{
    scene::asset_cache assets;
    const std::uint64_t retained_at_start = assets.statistics().retained_geometry_bytes;
    {
        scene::scene world{ assets };
        scene::snapshot_buffer buffer;

        const scene::mesh_handle mesh = make_mesh(assets);
        const scene::material_handle surface = make_material(assets);
        draw(world, create(world, 1), mesh, { surface });

        const auto publish = [&]
        {
            world.update_world_transforms();
            world.make_snapshot(buffer.begin_write());
            buffer.publish();
            world.end_tick();
        };

        publish(); // slot 1 holds version 1
        std::shared_ptr<const scene::mesh_data> first_version;
        {
            const scene::snapshot_reference front = buffer.acquire();
            REQUIRE(front);
            REQUIRE(front->meshes.size() == 1);
            CHECK(front->meshes[0].mesh.version == 1);
            first_version = front->meshes[0].mesh.data;
        }
        const std::uint64_t first_bytes = first_version->byte_size();
        first_version.reset(); // only the snapshot holds it now

        REQUIRE(assets.update_mesh(mesh, make_mesh_data(1, 2.0f)));
        CHECK(assets.current(mesh).version == 2);
        CHECK(assets.statistics().retained_geometry_bytes == retained_at_start + first_bytes);
        {
            // The renderer still reads version 1 from the front slot
            const scene::snapshot_reference front = buffer.acquire();
            CHECK(front->meshes[0].mesh.version == 1);
            CHECK(front->meshes[0].mesh.data->positions[1].x() == 1.0f);
        }

        publish(); // slot 0 holds version 2; slot 1 still holds version 1
        CHECK(assets.statistics().retained_geometry_bytes == retained_at_start + first_bytes);

        publish(); // slot 1 is reused: version 1 is freed
        CHECK(assets.statistics().retained_geometry_bytes == retained_at_start);
        {
            const scene::snapshot_reference front = buffer.acquire();
            CHECK(front->meshes[0].mesh.version == 2);
        }

        assets.release(mesh);
        assets.release(surface);
    }
    CHECK(assets.statistics().retained_geometry_bytes == retained_at_start);
    CHECK(assets.statistics().current_geometry_bytes == 0);
}

TEST_CASE("a steady-state make_snapshot allocates nothing", "[scene][unit]")
{
    scene::asset_cache assets;
    scene::scene world{ assets };

    const scene::mesh_handle mesh = make_mesh(assets, 3);
    const scene::texture_handle texture = make_texture(assets);
    scene::material_description textured;
    textured.base_color_texture = texture;
    const scene::material_handle first = make_material(assets, textured);
    const scene::material_handle second = make_material(assets);

    std::vector<entity> cars;
    for (scene::object_id id = 1; id <= 200; ++id)
    {
        const entity parent = id % 4 == 1 ? entt::null : cars.back();
        const entity created = create(world, id, entity_category::object, parent, at({ 1.0f, 0.0f, 0.0f }));
        draw(world, created, mesh, { first, second });
        if (id % 10 == 0)
        {
            world.registry().emplace<light>(created, light{ .kind = scene::light_kind::point });
        }
        cars.push_back(created);
    }

    scene::snapshot built;
    tick(world, built); // grows the vectors and the draw order
    REQUIRE(built.instances.size() == 600);

    bool allocated = false;
    {
        const tests::scoped_allocation_failure no_allocation;
        try
        {
            for (int build = 0; build < 100; ++build)
            {
                world.set_local(cars[build % 4 * 4], at({ static_cast<float>(build), 0.0f, 0.0f }));
                tick(world, built);
            }
        }
        catch (const std::bad_alloc&)
        {}
        allocated = no_allocation.triggered();
    }
    CHECK_FALSE(allocated);
    CHECK(built.instances.size() == 600);
    CHECK(built.lights.size() == 20);
    CHECK(built.textures.size() == 1);

    world.clear();
    assets.release(mesh);
    assets.release(first);
    assets.release(second);
    assets.release(texture);
}
