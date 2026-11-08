// The geometry budget (docs/tasks/12-visualizer.md §12.9, 12 §9.1): with a 10 MiB budget fed by lidar sweeps and belief
// geometry, the reductions happen in their order (drop the oldest sweeps, cut progressive meshes to a prefix, skip new
// geometry), the budget is never exceeded, the reduction level is reported, freeing room undoes them, and the asset
// cache's geometry bytes match the budget's count.

#include "helpers/recorded_assertions.h"
#include "representation/dynamic_mesh.h"
#include "representation/geometry_budget.h"
#include "representation/geometry_representation.h"
#include "representation/lidar_representation.h"
#include "representation/presentation_state.h"
#include "representation/representation_system.h"

#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/scene.h"

#include <catch2/catch_test_macros.hpp>

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
    constexpr std::uint64_t mebibyte = 1 << 20;
    constexpr std::uint32_t sweep_points = 100'000; // 16 bytes each with intensity colours: 1.6 MB a sweep

    constexpr scene::object_id driver_id(const std::uint64_t index) noexcept
    {
        return (std::uint64_t{ 1 } << 56) | index;
    }

    lidar_sweep make_sweep(const std::chrono::milliseconds time)
    {
        lidar_sweep sweep;
        sweep.capture_time = time;
        sweep.positions.resize(sweep_points, Eigen::Vector3f{ 1.0f, 2.0f, 0.0f });
        sweep.intensity.resize(sweep_points, 128);
        return sweep;
    }

    // Coloured points: 16 bytes a point
    geometry_chunk point_chunk(const std::uint32_t id, const std::uint32_t count)
    {
        geometry_chunk chunk;
        chunk.id = id;
        chunk.topology = geometry_topology::points;
        chunk.positions.resize(count, Eigen::Vector3f{ 0.0f, 0.0f, 1.0f });
        chunk.colors.resize(count, { 10, 200, 10, 255 });
        return chunk;
    }

    // Unlit, unindexed triangles without colours: 12 bytes a vertex
    geometry_chunk triangle_chunk(const std::uint32_t id, const std::uint32_t triangles)
    {
        geometry_chunk chunk;
        chunk.id = id;
        chunk.topology = geometry_topology::triangles;
        chunk.style.lit = false;
        for (std::uint32_t triangle = 0; triangle < triangles; ++triangle)
        {
            const auto x = static_cast<float>(triangle);
            chunk.positions.insert(
                chunk.positions.end(), { { x, 0.0f, 0.0f }, { x + 1.0f, 0.0f, 0.0f }, { x, 1.0f, 0.0f } });
        }
        return chunk;
    }

    struct budget_fixture
    {
        budget_fixture()
        {
            result<std::unique_ptr<representation::representation_system>> created =
                representation::representation_system::create(assets);
            REQUIRE(created);
            system = std::move(*created);
            state.grid = false;
            state.axes_gizmo = false;
            state.representation.geometry.budget_mib = 10;
            update();
            shared_bytes = assets.statistics().current_geometry_bytes; // the shared library: not in the budget
        }

        void update()
        {
            world.update_world_transforms();
            system->update(world, state);
            world.update_world_transforms();
        }

        scene::entity add(const std::uint64_t index, const scene::entity_category category)
        {
            const result<scene::entity> created = world.create_entity(
                { .id = driver_id(index), .name = "entity " + std::to_string(index), .category = category });
            REQUIRE(created);
            world.registry().emplace<driver_entity>(*created, driver_entity{ .category = category });
            return *created;
        }

        // The sweeps of a lidar that have a mesh, by capture time in milliseconds
        std::vector<std::int64_t> drawn_sweeps(const scene::entity lidar) const
        {
            std::vector<std::int64_t> drawn;
            const representation::lidar_representation& sweeps = system->lidar();
            const lidar_points& points = world.registry().get<lidar_points>(lidar);
            for (std::size_t index = 0; index < sweeps.sweep_count(lidar); ++index)
            {
                if (sweeps.sweep_mesh(lidar, index)->exists(world))
                {
                    drawn.push_back(
                        std::chrono::duration_cast<std::chrono::milliseconds>(points.sweeps[index].capture_time)
                            .count());
                }
            }
            return drawn;
        }

        // The budget's count is the cache's, and within the budget
        void check_bytes() const
        {
            const representation::geometry_reduction& reduction = system->reduction();
            CHECK(reduction.budget_bytes == 10 * mebibyte);
            CHECK(reduction.used_bytes <= reduction.budget_bytes);
            CHECK(assets.statistics().current_geometry_bytes - shared_bytes == reduction.used_bytes);
        }

        scene::asset_cache assets;
        scene::scene world{ assets };
        std::unique_ptr<representation::representation_system> system;
        representation::presentation_state state = representation::presentation_state::from_settings({});
        std::uint64_t shared_bytes = 0;
    };
} // namespace

TEST_CASE("representation: the geometry budget reduces in order and restores", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    budget_fixture fixture;
    entt::registry& registry = fixture.world.registry();

    // Two lidars keeping three sweeps each: 9.6 MB, within 10 MiB. Front's sweeps are older than rear's.
    const scene::entity front = fixture.add(1, scene::entity_category::sensor);
    const scene::entity rear = fixture.add(2, scene::entity_category::sensor);
    registry.emplace<lidar_points>(front, lidar_points{ .retention = 3 });
    registry.emplace<lidar_points>(rear, lidar_points{ .retention = 3 });
    for (int sweep = 1; sweep <= 3; ++sweep)
    {
        registry.patch<lidar_points>(front, [&](lidar_points& points) { points.push(make_sweep(sweep * 100ms)); });
        registry.patch<lidar_points>(
            rear, [&](lidar_points& points) { points.push(make_sweep(sweep * 100ms + 50ms)); });
    }
    fixture.update();
    CHECK_FALSE(fixture.system->reduction().reduced());
    CHECK(fixture.system->reduction().used_bytes == 6 * 1'600'000);
    CHECK(fixture.drawn_sweeps(front) == std::vector<std::int64_t>{ 100, 200, 300 });
    fixture.check_bytes();

    // 1. A 3.2 MB points chunk: the two oldest sweeps go, one from each lidar
    const scene::entity map = fixture.add(3, scene::entity_category::geometry);
    registry.emplace<belief_geometry>(map).replace(point_chunk(1, 200'000));
    fixture.update();
    CHECK(fixture.system->reduction().sweeps_dropped == 2);
    CHECK(fixture.system->reduction().prefix_fraction == 1.0f);
    CHECK(fixture.system->reduction().skipped == 0);
    CHECK(fixture.drawn_sweeps(front) == std::vector<std::int64_t>{ 200, 300 });
    CHECK(fixture.drawn_sweeps(rear) == std::vector<std::int64_t>{ 250, 350 });
    fixture.check_bytes();

    // 2. A 5 MB triangle chunk: down to the newest sweep of each lidar, then the largest progressive mesh (the points
    // chunk) is cut to a prefix while the sweeps stay whole
    const scene::entity area = fixture.add(4, scene::entity_category::geometry);
    registry.emplace<belief_geometry>(area).replace(triangle_chunk(1, 138'889));
    fixture.update();
    CHECK(fixture.system->reduction().sweeps_dropped == 4);
    CHECK(fixture.system->reduction().prefix_fraction < 1.0f);
    CHECK(fixture.system->reduction().skipped == 0);
    CHECK(fixture.drawn_sweeps(front) == std::vector<std::int64_t>{ 300 });
    CHECK(fixture.drawn_sweeps(rear) == std::vector<std::int64_t>{ 350 });
    const representation::dynamic_mesh* points = fixture.system->geometry().chunk_mesh(map, 1);
    REQUIRE(points != nullptr);
    CHECK(points->current_vertices < 200'000);
    CHECK(points->current_vertices >= 50'000);
    CHECK(points->current_vertices % representation::prefix_quantum == 0);
    CHECK(fixture.system->lidar().sweep_mesh(front, 2)->current_vertices == sweep_points);
    const float prefix = fixture.system->reduction().prefix_fraction;
    CHECK(
        prefix == static_cast<float>((2.0 * sweep_points + points->current_vertices) / (2.0 * sweep_points + 200'000)));
    fixture.check_bytes();

    // 3. A 4 MB triangle chunk doesn't fit even with every sweep dropped and every prefix at 25%: it's skipped, and
    // the rest stays as it was
    registry.emplace<belief_geometry>(fixture.add(5, scene::entity_category::geometry))
        .replace(triangle_chunk(1, 111'111));
    const std::uint64_t used = fixture.system->reduction().used_bytes;
    fixture.update();
    CHECK(fixture.system->reduction().skipped == 1);
    CHECK(fixture.system->reduction().sweeps_dropped == 4);
    CHECK(fixture.system->reduction().prefix_fraction == prefix);
    CHECK(fixture.system->reduction().used_bytes == used);
    const scene::entity late = fixture.world.find(driver_id(5)).value();
    CHECK(fixture.system->geometry().chunk_helper(late, 1) == entt::null);
    fixture.check_bytes();

    // Room returns: the 5 MB chunk goes. The skipped chunk comes in, the prefix is whole again, and the sweeps that
    // fit come back.
    registry.remove<belief_geometry>(area);
    fixture.update();
    CHECK(fixture.system->reduction().skipped == 0);
    CHECK(fixture.system->reduction().prefix_fraction == 1.0f);
    CHECK(fixture.system->geometry().chunk_helper(late, 1) != entt::null);
    CHECK(fixture.system->geometry().chunk_mesh(map, 1)->current_vertices == 200'000);
    CHECK(fixture.system->reduction().sweeps_dropped == 4);
    fixture.check_bytes();

    registry.remove<belief_geometry>(late);
    fixture.update();
    CHECK(fixture.system->reduction().sweeps_dropped == 2);
    CHECK(fixture.drawn_sweeps(front) == std::vector<std::int64_t>{ 200, 300 });
    fixture.check_bytes();

    registry.remove<belief_geometry>(map);
    fixture.update();
    CHECK_FALSE(fixture.system->reduction().reduced());
    CHECK(fixture.drawn_sweeps(front) == std::vector<std::int64_t>{ 100, 200, 300 });
    CHECK(fixture.drawn_sweeps(rear) == std::vector<std::int64_t>{ 150, 250, 350 });
    fixture.check_bytes();
    CHECK(assertions.count() == 0);
}

TEST_CASE("representation: a smaller budget releases what no longer fits", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    budget_fixture fixture;
    entt::registry& registry = fixture.world.registry();
    for (std::uint32_t index = 1; index <= 3; ++index)
    {
        registry.emplace<belief_geometry>(fixture.add(index, scene::entity_category::geometry))
            .replace(triangle_chunk(1, 83'334)); // 3 MB each
    }
    fixture.update();
    CHECK_FALSE(fixture.system->reduction().reduced());

    fixture.state.representation.geometry.budget_mib = 5;
    fixture.update();
    CHECK(fixture.system->reduction().skipped == 2);
    CHECK(fixture.system->reduction().used_bytes <= 5 * mebibyte);
    CHECK(
        fixture.assets.statistics().current_geometry_bytes - fixture.shared_bytes ==
        fixture.system->reduction().used_bytes);

    fixture.state.representation.geometry.budget_mib = 10;
    fixture.update();
    CHECK_FALSE(fixture.system->reduction().reduced());
    CHECK(assertions.count() == 0);
}

TEST_CASE("representation: the budget's plan executes releases, then shrinks, then growth", "[visualizer][unit]")
{
    using representation::budget_action;
    using representation::budget_item;
    using representation::budget_kind;

    // A progressive mesh at full size that must shrink, and a new chunk that grows into the room
    const std::vector<budget_item> items{
        { .kind = budget_kind::chunk,
          .progressive = true,
          .full_bytes = 160'000,
          .full_vertices = 10'000,
          .exists = true,
          .current_bytes = 160'000,
          .current_vertices = 10'000,
          .changed = false },
        { .kind = budget_kind::chunk, .full_bytes = 60'000 },
        { .kind = budget_kind::sweep,
          .progressive = true,
          .full_bytes = 1'600,
          .full_vertices = 100,
          .exists = true,
          .current_bytes = 1'600,
          .current_vertices = 100,
          .changed = false,
          .sensor = 1,
          .age = 1 },
        { .kind = budget_kind::sweep,
          .progressive = true,
          .full_bytes = 1'600,
          .full_vertices = 100,
          .sensor = 1,
          .age = 2 },
    };
    const representation::geometry_plan plan = representation::plan_geometry(items, 200'000);
    REQUIRE(plan.decisions.size() == 4);
    CHECK(plan.decisions[2].dropped); // the older sweep of the sensor
    CHECK(plan.decisions[2].action == budget_action::release);
    CHECK(plan.decisions[0].action == budget_action::build);
    CHECK(plan.decisions[0].vertices < 10'000);
    CHECK(plan.decisions[1].action == budget_action::build);
    CHECK(plan.decisions[3].action == budget_action::build);
    CHECK(plan.order.front() == 2);
    CHECK(plan.order[1] == 0);
    CHECK(plan.reduction.used_bytes <= 200'000);
    CHECK(plan.reduction.sweeps_dropped == 1);
    CHECK(plan.reduction.skipped == 0);
}
