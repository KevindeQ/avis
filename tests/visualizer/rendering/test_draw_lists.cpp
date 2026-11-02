// Draw lists (docs/tasks/11-renderer.md §11.8, 11 §4.1, §4.4–§4.6): records, culling per viewport, items, the sort and
// the group index. The unit tests run on hand-built snapshots and extraction results, without a device; the last test
// goes through the renderer.

#include "helpers/recorded_assertions.h"
#include "helpers/renderer_fixture.h"
#include "rendering/draw_lists.h"
#include "rendering/pipeline_variant_registry.h"
#include "rendering/scene_renderer.h"
#include "rendering/sort_key.h"

#include "avis/graphics/linear_arena.h"
#include "avis/scene/flags.h"
#include "avis/scene/snapshot.h"

#include <Eigen/Geometry>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;

namespace
{
    constexpr std::uint32_t width = 1280;
    constexpr std::uint32_t height = 720;

    // Materials by handle index
    constexpr std::uint32_t opaque_material = 1;
    constexpr std::uint32_t mask_material = 2;
    constexpr std::uint32_t blended_material = 3;
    constexpr std::uint32_t additive_material = 4;
    constexpr std::uint32_t subtractive_material = 5;

    constexpr scene::material_handle material(const std::uint32_t index) noexcept
    {
        return { .index = index, .generation = 1 };
    }

    draw_material material_state(const scene::material_handle handle)
    {
        switch (handle.index)
        {
        case mask_material:
            return { .alpha_mode = scene::alpha_mode::mask };
        case blended_material:
            return { .alpha_mode = scene::alpha_mode::blend };
        case additive_material:
            return { .alpha_mode = scene::alpha_mode::blend, .blend_operation = scene::blend_operation::additive };
        case subtractive_material:
            return { .alpha_mode = scene::alpha_mode::blend, .blend_operation = scene::blend_operation::subtractive };
        default:
            return {};
        }
    }

    // A set index computed from the key alone, so it doesn't depend on the order of the lookups (the tests' meshes
    // all have the same attributes)
    std::uint32_t pipeline_set_of(const pipeline_set_key& key)
    {
        const pipeline_set_key canonical = key.canonical();
        std::uint32_t index = static_cast<std::uint32_t>(canonical.shading_model);
        index = index * 3 + static_cast<std::uint32_t>(canonical.alpha_mode);
        index = index * 3 + static_cast<std::uint32_t>(canonical.blend_operation);
        index = index * 3 + static_cast<std::uint32_t>(canonical.topology);
        index = index * 2 + (canonical.double_sided ? 1u : 0u);
        index = index * 2 + (canonical.screen_aligned ? 1u : 0u);
        return index;
    }

    draw_list_lookup test_lookup()
    {
        return { .material = &material_state,
                 .pipeline_set = [](const pipeline_set_key& key) -> result<std::uint32_t>
                 { return pipeline_set_of(key); } };
    }

    Eigen::Matrix4f translation(const Eigen::Vector3f& position)
    {
        Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
        matrix.col(3).head<3>() = position;
        return matrix;
    }

    scene::aabb box(const float half_size)
    {
        return { .minimum = Eigen::Vector3f::Constant(-half_size), .maximum = Eigen::Vector3f::Constant(half_size) };
    }

    // A camera at the origin looking down −Z, 60° vertical field of view, 1280 × 720
    view_description test_view()
    {
        view_description view;
        view.name = "main";
        view.size = { width, height };
        view.camera.far_plane = 1000.0f;
        return view;
    }

    // A reverse-Z orthographic cascade over [x0, x1] × [y0, y1] of world space, looking down −Z from z = 100 to −100
    cascade_viewpoint orthographic_cascade(const float x0, const float x1, const float y0, const float y1)
    {
        constexpr float near_distance = 0.0f;
        constexpr float far_distance = 200.0f;
        cascade_viewpoint viewpoint;
        viewpoint.view_from_world = translation(Eigen::Vector3f{ 0.0f, 0.0f, -100.0f });
        Eigen::Matrix4f projection = Eigen::Matrix4f::Zero();
        projection(0, 0) = 2.0f / (x1 - x0);
        projection(0, 3) = -(x1 + x0) / (x1 - x0);
        projection(1, 1) = 2.0f / (y1 - y0);
        projection(1, 3) = -(y1 + y0) / (y1 - y0);
        // Depth 1 at the near plane (z_view = −near), 0 at the far one
        projection(2, 2) = 1.0f / (far_distance - near_distance);
        projection(2, 3) = far_distance / (far_distance - near_distance);
        projection(3, 3) = 1.0f;
        viewpoint.projection = projection;
        return viewpoint;
    }

    // A snapshot and its extraction result, built together so instance_sources line up
    struct test_scene
    {
        scene::snapshot snapshot;
        extraction_result extraction;
        std::vector<std::vector<scene::mesh_primitive>> primitives; // per mesh
        std::vector<draw_mesh> meshes;

        // A mesh with one submesh per bounds
        std::uint32_t add_mesh(
            const std::vector<scene::aabb>& submeshes,
            const scene::mesh_topology topology = scene::mesh_topology::triangles)
        {
            std::vector<scene::mesh_primitive> mesh_primitives;
            for (const scene::aabb& bounds : submeshes)
            {
                scene::mesh_primitive primitive;
                primitive.bounds = bounds;
                mesh_primitives.push_back(primitive);
            }
            primitives.push_back(std::move(mesh_primitives));
            meshes.push_back(
                { .topology = topology, .attributes = scene::vertex_attribute::normal | scene::vertex_attribute::uv0 });
            extraction.meshes.push_back(
                { .handle = { .index = static_cast<std::uint32_t>(meshes.size()), .generation = 1 }, .version = 1 });
            return static_cast<std::uint32_t>(meshes.size() - 1);
        }

        // Spans into `primitives`, once every mesh is added
        void finish()
        {
            for (std::size_t index = 0; index < meshes.size(); ++index)
            {
                meshes[index].primitives = primitives[index];
            }
        }

        scene::render_instance&
            add(const scene::object_id id,
                const std::uint32_t mesh,
                const std::uint32_t material_index,
                const Eigen::Vector3f& position,
                const std::uint32_t submesh = 0)
        {
            scene::render_instance instance;
            instance.object_id = id;
            instance.mesh = extraction.meshes[mesh].handle;
            instance.submesh = submesh;
            instance.material = material(material_index);
            instance.world_current = translation(position);
            instance.world_previous = instance.world_current;
            snapshot.instances.push_back(instance);
            extraction.instance_sources.push_back(static_cast<std::uint32_t>(snapshot.instances.size() - 1));

            gpu::instance_data data{};
            std::memcpy(&data.world, instance.world_current.data(), sizeof(data.world));
            data.mesh = mesh;
            data.material = 100 + material_index; // the slot
            data.object_id_low = static_cast<std::uint32_t>(id);
            data.object_id_high = static_cast<std::uint32_t>(id >> 32);
            extraction.instances.push_back(data);
            return snapshot.instances.back();
        }

        draw_list build(
            graphics::linear_arena& arena,
            const view_description& view = test_view(),
            const std::span<const cascade_viewpoint> cascades = {},
            const std::span<const draw_item> commands = {},
            const std::uint32_t bands = 16) const
        {
            const draw_list_sources sources{ .snapshot = snapshot,
                                             .extraction = extraction,
                                             .meshes = meshes,
                                             .cascades = cascades,
                                             .commands = commands,
                                             .lookup = test_lookup(),
                                             .opaque_depth_bands = bands };
            return build_draw_list(view, sources, arena);
        }
    };

    scene::object_id object_of(const test_scene& scene, const draw_list& list, const draw_item& item)
    {
        const draw_record& record = list.records()[item.value];
        return scene.snapshot.instances[scene.extraction.instance_sources[record.instance]].object_id;
    }

    // Whether a point is inside the clip volume of clip_from_world, by `margin` in NDC units
    enum class containment
    {
        inside,
        outside,
        near_boundary,
    };

    containment contains(const Eigen::Matrix4f& clip_from_world, const Eigen::Vector3f& point, const float margin)
    {
        const Eigen::Vector4f clip = clip_from_world * point.homogeneous();
        if (!(clip.w() > 0.0f))
        {
            return containment::outside;
        }
        // The margin is on x and y only: reverse-Z depth is below 0.01 past 10 m, and the tests' points are far from
        // the near and far planes
        const Eigen::Vector3f ndc = clip.head<3>() / clip.w();
        if (ndc.z() < 0.0f || ndc.z() > 1.0f)
        {
            return containment::outside;
        }
        const float distance = std::min(1.0f - std::abs(ndc.x()), 1.0f - std::abs(ndc.y()));
        if (std::abs(distance) < margin)
        {
            return containment::near_boundary;
        }
        return distance > 0.0f ? containment::inside : containment::outside;
    }
} // namespace

TEST_CASE("each visible instance is in the camera once and once per cascade it casts into", "[renderer][unit]")
{
    std::mt19937 random{ 1234 };
    std::uniform_real_distribution<float> coordinate{ -60.0f, 60.0f };
    std::uniform_int_distribution<int> choice{ 0, 9 };
    const std::array<std::uint32_t, 5> materials{
        opaque_material, mask_material, blended_material, additive_material, subtractive_material
    };

    test_scene scene;
    const std::uint32_t mesh = scene.add_mesh({ box(0.001f) });
    scene.finish();

    const view_description view = test_view();
    const Eigen::Matrix4f camera_clip =
        view.camera.projection(static_cast<float>(width) / static_cast<float>(height)) * view.camera.view_from_world();
    const std::array<cascade_viewpoint, 2> cascades{ orthographic_cascade(-30.0f, 0.0f, -30.0f, 30.0f),
                                                     orthographic_cascade(-10.0f, 50.0f, -50.0f, 10.0f) };

    // The viewports each object is expected in: 0 the camera, 1 + c cascade c
    std::map<scene::object_id, std::set<std::uint32_t>> expected;
    std::size_t expected_culled = 0;
    std::size_t expected_distance = 0;
    for (scene::object_id id = 1; scene.snapshot.instances.size() < 1000; ++id)
    {
        const Eigen::Vector3f position{ coordinate(random), coordinate(random), coordinate(random) };
        // Points within 1% of a boundary are left out: the boxes have a size, so they could be on either side
        const containment in_camera = contains(camera_clip, position, 0.01f);
        const containment in_cascade_0 =
            contains(cascades[0].projection * cascades[0].view_from_world, position, 0.01f);
        const containment in_cascade_1 =
            contains(cascades[1].projection * cascades[1].view_from_world, position, 0.01f);
        if (in_camera == containment::near_boundary || in_cascade_0 == containment::near_boundary ||
            in_cascade_1 == containment::near_boundary)
        {
            continue;
        }

        const std::uint32_t material_index = materials[static_cast<std::size_t>(choice(random)) % materials.size()];
        scene::render_instance& instance = scene.add(id, mesh, material_index, position);
        if (choice(random) < 3)
        {
            instance.flags = scene::instance_flags::visible | scene::instance_flags::pickable; // not a caster
        }
        const float distance = position.norm();
        bool beyond = false;
        if (choice(random) < 2)
        {
            instance.maximum_view_distance = 40.0f;
            beyond = std::abs(distance - 40.0f) > 0.1f && distance > 40.0f;
            if (std::abs(distance - 40.0f) <= 0.1f)
            {
                instance.maximum_view_distance = 0.0f; // too close to call
            }
        }

        std::set<std::uint32_t>& viewports = expected[id];
        if (in_camera == containment::inside)
        {
            if (beyond)
            {
                ++expected_distance;
            }
            else
            {
                viewports.insert(0);
            }
        }
        else
        {
            ++expected_culled;
        }
        const bool caster = static_cast<bool>(instance.flags & scene::instance_flags::casts_shadows) &&
                            material_state(instance.material).alpha_mode != scene::alpha_mode::blend;
        if (caster && in_cascade_0 == containment::inside)
        {
            viewports.insert(1);
        }
        if (caster && in_cascade_1 == containment::inside)
        {
            viewports.insert(2);
        }
    }

    graphics::linear_arena arena;
    const draw_list list = scene.build(arena, view, cascades);

    std::map<scene::object_id, std::multiset<std::uint32_t>> found;
    for (const draw_item& item : list.items())
    {
        REQUIRE_FALSE(is_command_key(item.key));
        found[object_of(scene, list, item)].insert(group_of(item.key).viewport.index);
    }
    for (const auto& [id, viewports] : expected)
    {
        const std::multiset<std::uint32_t> wanted(viewports.begin(), viewports.end());
        const auto entry = found.find(id);
        const std::multiset<std::uint32_t> got = entry != found.end() ? entry->second : std::multiset<std::uint32_t>{};
        INFO("object " << id);
        CHECK(got == wanted);
    }

    const draw_list_statistics& statistics = list.statistics();
    CHECK(statistics.records == 1000);
    CHECK(statistics.culled_frustum == expected_culled);
    CHECK(statistics.culled_distance == expected_distance);
    CHECK(statistics.items == statistics.camera_items + statistics.cascade_items[0] + statistics.cascade_items[1]);
    CHECK(statistics.camera_items > 50);
    CHECK(statistics.cascade_items[0] > 25);
    CHECK(statistics.cascade_items[1] > 25);
    CHECK(statistics.cascade_items[2] == 0);
}

TEST_CASE("draw list items take the group of 11 §4.2's mapping table", "[renderer][unit]")
{
    test_scene scene;
    const std::uint32_t mesh = scene.add_mesh({ box(0.5f) });
    scene.finish();

    const Eigen::Vector3f ahead{ 0.0f, 0.0f, -10.0f };
    scene.add(1, mesh, opaque_material, ahead);
    scene.add(2, mesh, mask_material, ahead);
    scene.add(3, mesh, blended_material, ahead);
    scene.add(4, mesh, additive_material, ahead);
    scene.add(5, mesh, subtractive_material, ahead);
    scene.add(6, mesh, opaque_material, ahead).layer = scene::draw_layer::overlay;
    scene.add(7, mesh, additive_material, ahead).layer = scene::draw_layer::overlay;
    scene.add(8, mesh, blended_material, ahead).layer = scene::draw_layer::overlay_on_top;

    graphics::linear_arena arena;
    const draw_list list = scene.build(arena);
    REQUIRE(list.items().size() == 8);

    std::map<scene::object_id, draw_key_fields> fields;
    for (const draw_item& item : list.items())
    {
        fields[object_of(scene, list, item)] = std::get<draw_key_fields>(decode_key(item.key));
    }
    const auto group = [](const viewport_layer layer, const translucency value)
    {
        return key_group{ .fullscreen_layer = fullscreen_layer::scene,
                          .viewport = viewport::camera(),
                          .viewport_layer = layer,
                          .translucency = value };
    };
    CHECK(fields[1].group == group(viewport_layer::world, translucency::opaque));
    CHECK(fields[2].group == group(viewport_layer::world, translucency::opaque));
    CHECK(fields[3].group == group(viewport_layer::world, translucency::blended));
    CHECK(fields[4].group == group(viewport_layer::world, translucency::additive));
    CHECK(fields[5].group == group(viewport_layer::world, translucency::subtractive));
    CHECK(fields[6].group == group(viewport_layer::overlay, translucency::opaque));
    CHECK(fields[7].group == group(viewport_layer::overlay, translucency::additive));
    CHECK(fields[8].group == group(viewport_layer::overlay_on_top, translucency::blended));

    // The material field: the pipeline set of the key, and the material slot
    CHECK(fields[1].material == 100 + opaque_material);
    CHECK(fields[1].pipeline_set == pipeline_set_of({ .alpha_mode = scene::alpha_mode::opaque }));
    CHECK(fields[2].pipeline_set == pipeline_set_of({ .alpha_mode = scene::alpha_mode::mask }));
    CHECK(
        fields[4].pipeline_set ==
        pipeline_set_of(
            { .alpha_mode = scene::alpha_mode::blend, .blend_operation = scene::blend_operation::additive }));

    // The depth field per group (11 §4.3), at a view depth of 10 m
    const std::uint32_t q = quantize_depth(10.0f);
    CHECK(fields[1].depth == opaque_depth_field(q, 16));
    CHECK(fields[3].depth == translucent_depth_field(q));
    CHECK(fields[6].depth == overlay_depth_field(0, false, q));
    CHECK(fields[7].depth == overlay_depth_field(0, true, q));
}

TEST_CASE("draw list depth fields order opaque bands, translucent depth and overlay priority", "[renderer][unit]")
{
    test_scene scene;
    const std::uint32_t mesh = scene.add_mesh({ box(0.1f) });
    scene.finish();

    // Translucent: far first. Opaque: near band first. Overlays: higher priority later.
    scene.add(1, mesh, blended_material, { 0.0f, 0.0f, -5.0f });
    scene.add(2, mesh, blended_material, { 0.0f, 0.0f, -50.0f });
    scene.add(3, mesh, opaque_material, { 0.0f, 0.0f, -500.0f });
    scene.add(4, mesh, opaque_material, { 0.0f, 0.0f, -2.0f });
    scene.add(5, mesh, opaque_material, { 0.0f, 0.0f, -5.0f }).layer = scene::draw_layer::overlay;
    scene.add(6, mesh, opaque_material, { 0.0f, 0.0f, -50.0f }).layer = scene::draw_layer::overlay;
    scene.snapshot.instances[4].layer_priority = 9;

    graphics::linear_arena arena;
    const draw_list list = scene.build(arena);
    std::vector<scene::object_id> order;
    for (const draw_item& item : list.items())
    {
        order.push_back(object_of(scene, list, item));
    }
    CHECK(order == std::vector<scene::object_id>{ 4, 3, 2, 1, 6, 5 });
}

TEST_CASE("draw list groups cover the list without gaps", "[renderer][unit]")
{
    test_scene scene;
    const std::uint32_t mesh = scene.add_mesh({ box(0.5f) });
    scene.finish();

    std::mt19937 random{ 99 };
    std::uniform_real_distribution<float> lateral{ -5.0f, 5.0f };
    std::uniform_real_distribution<float> depth{ -80.0f, -2.0f };
    std::uniform_int_distribution<std::uint32_t> material_index{ opaque_material, subtractive_material };
    std::uniform_int_distribution<int> layer{ 0, 2 };
    for (scene::object_id id = 1; id <= 300; ++id)
    {
        scene.add(id, mesh, material_index(random), { lateral(random), lateral(random), depth(random) }).layer =
            static_cast<scene::draw_layer>(layer(random));
    }
    const std::array<cascade_viewpoint, 1> cascades{ orthographic_cascade(-10.0f, 10.0f, -10.0f, 10.0f) };

    // Two commands: one after the opaque world draws, one in a group without draws
    const std::array<draw_item, 2> commands{
        draw_item{ .key = make_command_key(
                       { .group = { .viewport_layer = viewport_layer::world },
                         .command = builtin_command{ .opcode = command_opcode::reset_state } }),
                   .value = 0 },
        draw_item{ .key = make_command_key(
                       { .group = { .fullscreen_layer = fullscreen_layer::fullscreen_effects,
                                    .viewport_layer = viewport_layer::background },
                         .command = builtin_command{ .opcode = command_opcode::reset_state } }),
                   .value = 16 },
    };

    graphics::linear_arena arena;
    const draw_list list = scene.build(arena, test_view(), cascades, commands);
    const std::span<const draw_item> items = list.items();
    REQUIRE(std::ranges::is_sorted(items));
    CHECK(list.statistics().command_items == 2);

    std::uint32_t next = 0;
    for (const draw_group& group : list.groups())
    {
        CHECK(group.first == next);
        REQUIRE(group.count > 0);
        for (std::uint32_t index = group.first; index < group.first + group.count; ++index)
        {
            CHECK(group_of(items[index].key) == group.group);
        }
        const std::span<const draw_item> range = list.range(key_prefix(group.group));
        CHECK(range.data() == items.data() + group.first);
        CHECK(range.size() == group.count);
        next = group.first + group.count;
    }
    CHECK(next == items.size());
    CHECK(list.groups().size() == list.statistics().groups);

    // The opaque world group ends with its command
    const std::span<const draw_item> opaque_world =
        list.range(key_prefix({ .viewport_layer = viewport_layer::world, .translucency = translucency::opaque }));
    REQUIRE_FALSE(opaque_world.empty());
    CHECK(opaque_world.back().key == commands[0].key);
    CHECK(list.range(key_prefix(fullscreen_layer::fullscreen_effects)).size() == 1);

    // A pass's span of consecutive groups (blended … subtractive) is the sum of its groups
    const auto translucent_group = [](const translucency value)
    { return key_prefix({ .viewport_layer = viewport_layer::world, .translucency = value }); };
    CHECK(
        list.range(prefix_range(translucent_group(translucency::blended), translucent_group(translucency::subtractive)))
            .size() == list.range(translucent_group(translucency::blended)).size() +
                           list.range(translucent_group(translucency::additive)).size() +
                           list.range(translucent_group(translucency::subtractive)).size());

    // Absent prefixes give empty spans
    CHECK(list.range(key_prefix(fullscreen_layer::hud)).empty());
    CHECK(list.range(key_prefix(fullscreen_layer::scene, viewport::shadow_cascade(3))).empty());
    CHECK(list.range(key_prefix(fullscreen_layer::scene, viewport::camera(), viewport_layer::effects)).empty());

    // An empty list has no groups
    test_scene empty;
    empty.finish();
    const draw_list nothing = empty.build(arena);
    CHECK(nothing.items().empty());
    CHECK(nothing.groups().empty());
    CHECK(nothing.range(key_prefix(fullscreen_layer::scene)).empty());
}

TEST_CASE("draw records are mesh-major: equal keys keep a mesh's records together", "[renderer][unit]")
{
    test_scene scene;
    const std::uint32_t first = scene.add_mesh({ box(0.5f) });
    const std::uint32_t second = scene.add_mesh({ box(0.5f), box(0.25f) });
    scene.finish();

    // One material at one depth band: every item has the same key. The input interleaves the meshes and submeshes.
    std::mt19937 random{ 7 };
    std::uniform_int_distribution<int> pick{ 0, 2 };
    for (scene::object_id id = 1; id <= 120; ++id)
    {
        const int which = pick(random);
        const Eigen::Vector3f position{ 0.0f, 0.0f, -10.0f };
        if (which == 0)
        {
            scene.add(id, first, opaque_material, position);
        }
        else
        {
            scene.add(id, second, opaque_material, position, static_cast<std::uint32_t>(which - 1));
        }
    }

    graphics::linear_arena arena;
    const draw_list list = scene.build(arena);
    const std::span<const draw_item> items = list.items();
    REQUIRE(items.size() == 120);
    REQUIRE(std::ranges::all_of(items, [&](const draw_item& item) { return item.key == items[0].key; }));

    // Each (mesh, submesh) is one contiguous run, and within it the objects are in id order
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    std::pair<std::uint32_t, std::uint32_t> current{ ~0u, ~0u };
    scene::object_id previous_id = 0;
    for (const draw_item& item : items)
    {
        const draw_record& record = list.records()[item.value];
        const std::pair<std::uint32_t, std::uint32_t> pair{ record.mesh, record.submesh };
        if (pair != current)
        {
            CHECK(seen.insert(pair).second);
            current = pair;
            previous_id = 0;
        }
        const scene::object_id id = object_of(scene, list, item);
        CHECK(id > previous_id);
        previous_id = id;
    }
    CHECK(seen.size() == 3);
}

TEST_CASE("a draw list doesn't depend on the order of its input instances", "[renderer][unit]")
{
    std::mt19937 random{ 2024 };
    std::uniform_real_distribution<float> lateral{ -20.0f, 20.0f };
    std::uniform_real_distribution<float> depth{ -100.0f, -1.0f };
    std::uniform_int_distribution<std::uint32_t> material_index{ opaque_material, subtractive_material };
    std::uniform_int_distribution<int> small{ 0, 3 };

    struct placement
    {
        scene::object_id id;
        std::uint32_t mesh;
        std::uint32_t material;
        Eigen::Vector3f position;
        scene::draw_layer layer;
    };
    std::vector<placement> placements;
    for (scene::object_id id = 1; id <= 500; ++id)
    {
        // Some share a position, so their keys are equal and only the records order them
        const Eigen::Vector3f position = small(random) == 0 ?
                                             Eigen::Vector3f{ 0.0f, 0.0f, -10.0f } :
                                             Eigen::Vector3f{ lateral(random), lateral(random), depth(random) };
        placements.push_back(
            { id,
              static_cast<std::uint32_t>(small(random) % 2),
              material_index(random),
              position,
              static_cast<scene::draw_layer>(small(random) % 3) });
    }
    const std::array<cascade_viewpoint, 2> cascades{ orthographic_cascade(-20.0f, 0.0f, -20.0f, 20.0f),
                                                     orthographic_cascade(0.0f, 20.0f, -20.0f, 20.0f) };

    const auto build = [&](const std::vector<placement>& order, graphics::linear_arena& arena)
    {
        test_scene scene;
        scene.add_mesh({ box(0.5f) });
        scene.add_mesh({ box(1.0f) });
        scene.finish();
        for (const placement& item : order)
        {
            scene.add(item.id, item.mesh, item.material, item.position).layer = item.layer;
        }
        const draw_list list = scene.build(arena, test_view(), cascades);
        std::vector<std::tuple<std::uint64_t, scene::object_id, std::uint32_t>> result;
        for (const draw_item& item : list.items())
        {
            result.emplace_back(item.key, object_of(scene, list, item), item.value);
        }
        return result;
    };

    graphics::linear_arena first_arena;
    graphics::linear_arena second_arena;
    const auto first = build(placements, first_arena);
    std::ranges::shuffle(placements, random);
    const auto second = build(placements, second_arena);
    CHECK(first.size() > 400);
    CHECK(first == second);
}

TEST_CASE(
    "a screen-aligned anchor just outside the frustum whose pixel bounds reach inside is kept", "[renderer][unit]")
{
    test_scene scene;
    // A label 100 pixels wide: 50 pixels on each side of its anchor
    scene::aabb label;
    label.minimum = Eigen::Vector3f{ -50.0f, -10.0f, 0.0f };
    label.maximum = Eigen::Vector3f{ 50.0f, 10.0f, 0.0f };
    const std::uint32_t mesh = scene.add_mesh({ label });
    scene.finish();

    const view_description view = test_view();
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float half_width_at_10 = 10.0f * std::tan(0.5f * view.camera.vertical_field_of_view) * aspect;
    const float metres_per_pixel = 2.0f * half_width_at_10 / static_cast<float>(width);

    const auto add_label = [&](const scene::object_id id, const float pixels_outside)
    {
        const Eigen::Vector3f anchor{ half_width_at_10 + pixels_outside * metres_per_pixel, 0.0f, -10.0f };
        scene::render_instance& instance = scene.add(id, mesh, opaque_material, anchor);
        instance.layer = scene::draw_layer::overlay;
        instance.flags = scene::default_instance_flags | scene::instance_flags::screen_aligned;
    };
    add_label(1, 20.0f); // 20 px outside: its left half reaches 30 px in
    add_label(2, 80.0f); // 80 px outside: 30 px short of the edge
    add_label(3, -100.0f); // inside

    graphics::linear_arena arena;
    const draw_list list = scene.build(arena, view);
    std::set<scene::object_id> drawn;
    for (const draw_item& item : list.items())
    {
        drawn.insert(object_of(scene, list, item));
        CHECK(
            std::get<draw_key_fields>(decode_key(item.key)).pipeline_set ==
            pipeline_set_of({ .screen_aligned = true }));
    }
    CHECK(drawn == std::set<scene::object_id>{ 1, 3 });
    CHECK(list.statistics().culled_frustum == 1);

    // A higher device pixel ratio makes the label wider on screen: the second one reaches in at 2x
    view_description high_density = view;
    high_density.device_pixel_ratio = 2.0f;
    const draw_list dense = scene.build(arena, high_density);
    CHECK(dense.statistics().camera_items == 3);

    // Behind the camera, or in front of the near plane, a label is culled whatever its size
    CHECK_FALSE(screen_aligned_visible(
        Eigen::Matrix4f::Identity() * view.camera.projection(aspect),
        Eigen::Vector3f{ 0.0f, 0.0f, 5.0f },
        label,
        width,
        height,
        1.0f));
    CHECK_FALSE(screen_aligned_visible(
        view.camera.projection(aspect), Eigen::Vector3f{ 0.0f, 0.0f, -0.05f }, label, width, height, 1.0f));
}

TEST_CASE("screen-aligned instances in the world layer fail the debug check and get no record", "[renderer][unit]")
{
    tests::recorded_assertions assertions;
    test_scene scene;
    const std::uint32_t mesh = scene.add_mesh({ box(0.5f) });
    scene.finish();
    scene.add(1, mesh, opaque_material, { 0.0f, 0.0f, -10.0f }).flags =
        scene::default_instance_flags | scene::instance_flags::screen_aligned;
    scene.add(2, mesh, opaque_material, { 0.0f, 0.0f, -10.0f });
    // A submesh the mesh doesn't have
    scene.add(3, mesh, opaque_material, { 0.0f, 0.0f, -10.0f }, 4);

    graphics::linear_arena arena;
    const draw_list list = scene.build(arena);
    CHECK(list.statistics().skipped_invalid == 2);
    CHECK(list.statistics().records == 1);
    REQUIRE(list.items().size() == 1);
    CHECK(object_of(scene, list, list.items()[0]) == 2);
#ifndef NDEBUG
    CHECK(assertions.count() == 1);
    CHECK(assertions.contains("screen_aligned"));
#endif
}

TEST_CASE("instances whose pipeline set lookup fails get no item", "[renderer][unit]")
{
    test_scene scene;
    const std::uint32_t mesh = scene.add_mesh({ box(0.5f) });
    scene.finish();
    scene.add(1, mesh, opaque_material, { 0.0f, 0.0f, -10.0f });
    scene.add(2, mesh, blended_material, { 0.0f, 0.0f, -10.0f });

    draw_list_sources sources{ .snapshot = scene.snapshot,
                               .extraction = scene.extraction,
                               .meshes = scene.meshes,
                               .cascades = {},
                               .commands = {},
                               .lookup = test_lookup() };
    sources.lookup.pipeline_set = [](const pipeline_set_key& key) -> result<std::uint32_t>
    {
        if (key.alpha_mode == scene::alpha_mode::blend)
        {
            return std::unexpected{ avis::make_error(generic_error_kind::invalid_configuration, "no pipeline") };
        }
        return 0u;
    };

    graphics::linear_arena arena;
    const draw_list list = build_draw_list(test_view(), sources, arena);
    CHECK(list.statistics().skipped_no_pipeline == 1);
    REQUIRE(list.items().size() == 1);
    CHECK(object_of(scene, list, list.items()[0]) == 1);
}

TEST_CASE("sort_draw_items gives the order of std::ranges::sort", "[renderer][unit]")
{
    std::mt19937_64 random{ 31 };
    graphics::linear_arena arena;
    // Sizes on both sides of the radix threshold (512)
    for (const std::size_t size : { 0u, 1u, 2u, 100u, 511u, 512u, 3000u, 20000u })
    {
        // Few distinct keys, so many items tie on the key and only the value orders them; some bytes vary only in
        // part of the items, some in none
        std::uniform_int_distribution<std::uint64_t> key_choice{ 0, 40 };
        std::vector<std::uint64_t> keys;
        for (int index = 0; index < 41; ++index)
        {
            keys.push_back((random() & 0x00FF'FFFF'0000'FF00ull) | (std::uint64_t{ 1 } << 62));
        }
        std::vector<draw_item> items;
        for (std::size_t index = 0; index < size; ++index)
        {
            // Values are unique, as the draw list's are, and span more than one byte
            items.push_back({ .key = keys[key_choice(random)], .value = static_cast<std::uint32_t>(index * 977u) });
        }
        std::ranges::shuffle(items, random);

        std::vector<draw_item> expected = items;
        std::ranges::sort(expected);
        sort_draw_items(items, arena);
        INFO("size " << size);
        CHECK(items == expected);
    }

    // Items that differ only in their value's high byte, and keys that differ only in their top byte
    std::vector<draw_item> high_bytes;
    for (std::uint32_t index = 0; index < 1024; ++index)
    {
        high_bytes.push_back({ .key = std::uint64_t{ (index * 7u) % 3u } << 56, .value = (1023u - index) << 24 });
    }
    std::vector<draw_item> expected = high_bytes;
    std::ranges::sort(expected);
    sort_draw_items(high_bytes, arena);
    CHECK(high_bytes == expected);
}

TEST_CASE("frustum planes cull boxes outside and keep boxes across a plane", "[renderer][unit]")
{
    const view_description view = test_view();
    const frustum camera{ view.camera.projection(16.0f / 9.0f) * view.camera.view_from_world() };

    scene::aabb ahead = box(1.0f);
    ahead = ahead.transformed(Eigen::Affine3f{ Eigen::Translation3f{ 0.0f, 0.0f, -10.0f } });
    CHECK(camera.intersects(ahead));
    CHECK_FALSE(camera.intersects(box(1.0f).transformed(Eigen::Affine3f{ Eigen::Translation3f{ 0.0f, 0.0f, 10.0f } })));
    // Straddling the near plane (0.1 m): kept
    CHECK(camera.intersects(box(1.0f)));
    // Beyond the far plane (1 km)
    CHECK_FALSE(
        camera.intersects(box(1.0f).transformed(Eigen::Affine3f{ Eigen::Translation3f{ 0.0f, 0.0f, -1100.0f } })));
    CHECK_FALSE(camera.intersects(scene::aabb{}));

    // An infinite far plane has no far plane to cull against
    view_description infinite = view;
    infinite.camera.far_plane.reset();
    const frustum unbounded{ infinite.camera.projection(16.0f / 9.0f) * infinite.camera.view_from_world() };
    CHECK(unbounded.intersects(box(1.0f).transformed(Eigen::Affine3f{ Eigen::Translation3f{ 0.0f, 0.0f, -1e6f } })));
}

TEST_CASE("draw_material_of reads a material slot's state", "[renderer][unit]")
{
    gpu::material_data data{};
    CHECK(draw_material_of(data) == draw_material{});
    data.shading_model = gpu::shading_model_unlit;
    data.alpha_mode = gpu::alpha_mode_blend;
    data.blend_operation = gpu::blend_operation_subtractive;
    data.flags = gpu::material_flag_double_sided | gpu::material_flag_use_vertex_color;
    CHECK(
        draw_material_of(data) == draw_material{ .shading_model = scene::shading_model::unlit,
                                                 .alpha_mode = scene::alpha_mode::blend,
                                                 .blend_operation = scene::blend_operation::subtractive,
                                                 .double_sided = true });
    data.alpha_mode = gpu::alpha_mode_mask;
    CHECK(draw_material_of(data).alpha_mode == scene::alpha_mode::mask);
}

TEST_CASE("10,000 instances with one or two cascades build and sort in under 1.5 ms", "[renderer][unit][!benchmark][.]")
{
    std::mt19937 random{ 5 };
    std::uniform_real_distribution<float> lateral{ -60.0f, 60.0f };
    std::uniform_real_distribution<float> depth{ -150.0f, -1.0f };
    std::uniform_int_distribution<std::uint32_t> material_index{ opaque_material, subtractive_material };

    test_scene scene;
    for (int mesh = 0; mesh < 50; ++mesh)
    {
        scene.add_mesh({ box(0.5f) });
    }
    scene.finish();
    std::uniform_int_distribution<std::uint32_t> mesh_index{ 0, 49 };
    for (scene::object_id id = 1; id <= 10'000; ++id)
    {
        // Mostly opaque, as a scene is
        const std::uint32_t material_slot = id % 5 == 0 ? material_index(random) : opaque_material;
        scene.add(id, mesh_index(random), material_slot, { lateral(random), lateral(random), depth(random) });
    }
    // Two overlapping cascades: the near casters are in both
    const std::array<cascade_viewpoint, 2> cascades{ orthographic_cascade(-40.0f, 40.0f, -40.0f, 40.0f),
                                                     orthographic_cascade(-80.0f, 80.0f, -80.0f, 80.0f) };

    graphics::linear_arena arena;
    constexpr int iterations = 100;
    std::chrono::nanoseconds total{};
    std::size_t items = 0;
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        arena.reset();
        const auto start = std::chrono::steady_clock::now();
        const draw_list list = scene.build(arena, test_view(), cascades);
        total += std::chrono::steady_clock::now() - start;
        items = list.items().size();
    }

    const double average = std::chrono::duration<double, std::milli>(total).count() / iterations;
    WARN("average draw list build of 10,000 instances: " << average << " ms for " << items << " items");

    // For comparison: the sort step alone, and std::ranges::sort on the same shuffled items (11 §4.5)
    const draw_list list = scene.build(arena, test_view(), cascades);
    std::vector<draw_item> shuffled(list.items().begin(), list.items().end());
    std::chrono::nanoseconds radix_total{};
    std::chrono::nanoseconds comparison_total{};
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        std::ranges::shuffle(shuffled, random);
        std::vector<draw_item> copy = shuffled;
        auto start = std::chrono::steady_clock::now();
        sort_draw_items(copy, arena);
        radix_total += std::chrono::steady_clock::now() - start;

        copy = shuffled;
        start = std::chrono::steady_clock::now();
        std::ranges::sort(copy);
        comparison_total += std::chrono::steady_clock::now() - start;
    }
    const double radix = std::chrono::duration<double, std::milli>(radix_total).count() / iterations;
    const double comparison = std::chrono::duration<double, std::milli>(comparison_total).count() / iterations;
    WARN(
        "sort of " << shuffled.size() << " items: " << radix << " ms (sort_draw_items), " << comparison
                   << " ms (std::ranges::sort)");
#ifdef NDEBUG
    CHECK(average < 1.5);
#endif
}

TEST_CASE_METHOD(
    tests::renderer_fixture, "the renderer builds a draw list per view from the frame's instances", "[renderer]")
{
    add_unit_cube({ 0.0f, 0.0f, -5.0f });
    add_unit_cube({ 2.0f, 0.0f, -5.0f });
    scene::material_description glass;
    glass.alpha = scene::alpha_mode::blend;
    glass.base_color_factor.alpha = 0.5f;
    add_unit_cube({ -2.0f, 0.0f, -5.0f }, glass);
    add_unit_cube({ 0.0f, 0.0f, 5.0f }); // behind the main camera
    publish();

    // The second view looks the other way: only the cube behind the main camera is in it
    visualizer::rendering::view_description back = view("back", make_output({ 64, 36 }));
    back.camera.world_from_view = Eigen::AngleAxisf{ std::numbers::pi_v<float>, Eigen::Vector3f::UnitY() };
    const std::vector views{ view(), back };
    render_and_read(views);

    const std::span<const draw_list> lists = renderer->draw_lists();
    REQUIRE(lists.size() == 2);
    const draw_list& main = lists[0];
    CHECK(main.statistics().records == 4);
    CHECK(main.statistics().camera_items == 3);
    CHECK(main.statistics().culled_frustum == 1);
    CHECK(main.statistics().skipped_no_pipeline == 0);
    CHECK(lists[1].statistics().camera_items == 1);

    // The keys name the registry's sets for each material, in the world groups of 11 §4.2
    const pipeline_variant_registry& registry = renderer->pipeline_variants();
    const std::span<const draw_item> opaque =
        main.range(key_prefix({ .viewport_layer = viewport_layer::world, .translucency = translucency::opaque }));
    const std::span<const draw_item> blended =
        main.range(key_prefix({ .viewport_layer = viewport_layer::world, .translucency = translucency::blended }));
    REQUIRE(opaque.size() == 2);
    REQUIRE(blended.size() == 1);
    for (const draw_item& item : opaque)
    {
        const draw_key_fields fields = std::get<draw_key_fields>(decode_key(item.key));
        REQUIRE(fields.pipeline_set < registry.size());
        CHECK(registry.set(fields.pipeline_set).key.alpha_mode == scene::alpha_mode::opaque);
        CHECK(registry.set(fields.pipeline_set).key.topology == scene::mesh_topology::triangles);
    }
    const draw_key_fields glass_fields = std::get<draw_key_fields>(decode_key(blended[0].key));
    CHECK(registry.set(glass_fields.pipeline_set).key.alpha_mode == scene::alpha_mode::blend);
    CHECK(glass_fields.material != std::get<draw_key_fields>(decode_key(opaque[0].key)).material);

    // The debug information has the lists' counts, per view
    const renderer_debug_information information = renderer->debug_information();
    REQUIRE(information.draw_lists.size() == 2);
    CHECK(information.draw_lists[0].camera_items == 3);
    CHECK(information.draw_lists[1].camera_items == 1);
}
