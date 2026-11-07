// The simulation thread's presentation path and the demo scene (docs/tasks/12-visualizer.md §12.6): coalesced
// republishes that keep the step fields, Reset, selection flags, the GUI's updates and their rates, and 1,000 demo
// ticks. simulation_loop runs on the test thread with a made-up clock; one test runs the real thread.

#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"
#include "representation/presentation_state.h"
#include "representation/scene_representation.h"
#include "simulation/demo_scene.h"
#include "simulation/gui_sink.h"
#include "simulation/simulation_command.h"
#include "simulation/simulation_thread.h"

#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/scene.h"
#include "avis/scene/snapshot.h"
#include "avis/threading/thread_pool.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

using avis::result;
namespace representation = avis::visualizer::representation;
namespace scene = avis::scene;
namespace simulation = avis::visualizer::simulation;
namespace tests = avis::tests;
namespace threading = avis::threading;
using namespace avis::scene::components;
using namespace std::chrono_literals;
using simulation::demo_scene;
using simulation::simulation_loop;
using clock_type = simulation_loop::clock;

namespace
{
    // Well after the clock's epoch, so the loop's first rate-limited posts are due at once
    const clock_type::time_point start_time = clock_type::time_point{ 1h };

    // Records every call with the made-up time the test is at
    class recording_sink final : public simulation::gui_sink
    {
    public:
        explicit recording_sink(const clock_type::time_point& now) : now_{ now } {}

        void tree_changed(std::vector<scene::tree_change> changes) override
        {
            trees.push_back(std::move(changes));
        }

        void selected_entity_changed(std::optional<scene::reflected_entity> entity) override
        {
            selected.emplace_back(now_, std::move(entity));
        }

        void statistics_changed(const scene::scene_statistics& values) override
        {
            statistics.emplace_back(now_, values);
        }

        void follow_target_changed(const simulation::follow_target& target) override
        {
            follows.push_back(target);
        }

        // Every change of `kind` for `id` received so far
        std::size_t count(const scene::tree_change_kind kind, const scene::object_id id) const
        {
            std::size_t found = 0;
            for (const auto& batch : trees)
            {
                found += static_cast<std::size_t>(std::ranges::count_if(
                    batch, [&](const scene::tree_change& change) { return change.kind == kind && change.id == id; }));
            }
            return found;
        }

        std::vector<std::vector<scene::tree_change>> trees;
        std::vector<std::pair<clock_type::time_point, std::optional<scene::reflected_entity>>> selected;
        std::vector<std::pair<clock_type::time_point, scene::scene_statistics>> statistics;
        std::vector<simulation::follow_target> follows;

    private:
        const clock_type::time_point& now_;
    };

    // Stands in for task 12.8's representation: every entity with a display shape gets one shared triangle mesh, so
    // the snapshots have instances
    class fake_representation final : public representation::scene_representation
    {
    public:
        explicit fake_representation(scene::asset_cache& assets) : assets_{ assets }
        {
            scene::mesh_builder builder{ scene::mesh_topology::triangles };
            builder.add_vertex({ 0.0f, 0.0f, 0.0f }).add_vertex({ 1.0f, 0.0f, 0.0f }).add_vertex({ 0.0f, 1.0f, 0.0f });
            builder.end_primitive();
            result<scene::mesh_data> data = std::move(builder).build();
            REQUIRE(data);
            result<scene::mesh_handle> created_mesh = assets.create_mesh(std::move(*data));
            REQUIRE(created_mesh);
            mesh_ = *created_mesh;
            result<scene::material_handle> created_material = assets.create_material({});
            REQUIRE(created_material);
            material_ = *created_material;
        }

        ~fake_representation() override
        {
            assets_.release(mesh_);
            assets_.release(material_);
        }

        fake_representation(const fake_representation&) = delete;
        fake_representation& operator=(const fake_representation&) = delete;

        void update(scene::scene& target, const representation::presentation_state& state) override
        {
            ++calls;
            last_stale = state.stale;
            entt::registry& registry = target.registry();
            std::vector<scene::entity> bare;
            for (const scene::entity entity : registry.view<display_shape>(entt::exclude<mesh>))
            {
                bare.push_back(entity);
            }
            for (const scene::entity entity : bare)
            {
                registry.emplace<mesh>(entity, mesh{ .asset = mesh_ });
                registry.emplace<material>(entity, material{ .materials = { material_ } });
            }
        }

        std::size_t calls = 0;
        bool last_stale = false;

    private:
        scene::asset_cache& assets_;
        scene::mesh_handle mesh_;
        scene::material_handle material_;
    };

    // A loop on the test thread, at a time the test moves
    struct loop_fixture
    {
        explicit loop_fixture(const bool demo, const bool with_representation = true)
        {
            if (with_representation)
            {
                representation.emplace(assets);
            }
            simulation::simulation_settings settings;
            settings.demo_scene = demo;
            loop.emplace(assets, snapshots, sink, settings, now, representation ? &*representation : nullptr);
        }

        // Moves the time on and runs what's due
        void advance_to(const clock_type::time_point time)
        {
            now = time;
            loop->update(now);
        }

        void handle(const simulation::simulation_command& command)
        {
            loop->handle(command, now);
        }

        // The ego vehicle's instance in the newest snapshot
        const scene::render_instance* ego_instance(const scene::snapshot& snapshot) const
        {
            const auto found =
                std::ranges::find(snapshot.instances, demo_scene::ego_vehicle_id, &scene::render_instance::object_id);
            return found == snapshot.instances.end() ? nullptr : &*found;
        }

        clock_type::time_point now = start_time;
        scene::asset_cache assets;
        scene::snapshot_buffer snapshots;
        recording_sink sink{ now };
        std::optional<fake_representation> representation;
        std::optional<simulation_loop> loop;
    };

    std::size_t driver_entities(scene::scene& target)
    {
        return target.registry().view<driver_entity>().size();
    }
} // namespace

TEST_CASE("simulation loop: a burst of presentation commands gives one republish", "[visualizer][unit]")
{
    loop_fixture fixture{ true };
    fixture.advance_to(start_time); // tick 1
    fixture.advance_to(start_time + demo_scene::tick_interval); // tick 2: the ego moved since tick 1
    REQUIRE(fixture.loop->step() == 2);

    scene::snapshot before;
    {
        const scene::snapshot_reference reference = fixture.snapshots.acquire();
        REQUIRE(reference);
        before = *reference;
    }
    REQUIRE(fixture.ego_instance(before) != nullptr);
    CHECK_FALSE(fixture.ego_instance(before)->world_previous.isApprox(fixture.ego_instance(before)->world_current));

    // 100 commands within 10 ms, with the loop running between them as the thread would
    const std::uint64_t publishes = fixture.loop->publishes();
    const clock_type::time_point burst = start_time + demo_scene::tick_interval + 5ms;
    for (int index = 0; index < 100; ++index)
    {
        fixture.advance_to(burst + index * 100us);
        fixture.handle(
            index % 2 == 0 ? simulation::simulation_command{ simulation::set_staleness{ .enabled = index % 4 == 0 } } :
                             simulation::simulation_command{ simulation::set_category_labels{
                                 .category = scene::entity_category::object, .enabled = index % 3 == 0 } });
    }
    CHECK(fixture.loop->publishes() == publishes); // nothing yet: the window is 16 ms from the first command

    fixture.advance_to(burst + simulation_loop::republish_interval);
    CHECK(fixture.loop->publishes() == publishes + 1);
    fixture.advance_to(burst + 3 * simulation_loop::republish_interval);
    CHECK(fixture.loop->publishes() == publishes + 1); // nothing left to republish
    CHECK(fixture.loop->step() == 2); // no tick happened in between
    CHECK(fixture.representation->last_stale == fixture.loop->presentation().stale);

    const scene::snapshot_reference after = fixture.snapshots.acquire();
    REQUIRE(after);
    CHECK(after->publish_sequence == before.publish_sequence + 1);
    CHECK(after->step == before.step);
    CHECK(after->time_previous == before.time_previous);
    CHECK(after->time_current == before.time_current);
    CHECK(after->anchor.t_base == before.anchor.t_base);
    CHECK(after->anchor.wall_base == before.anchor.wall_base);
    CHECK(after->anchor.rate == before.anchor.rate);
    const scene::render_instance* ego = fixture.ego_instance(*after);
    REQUIRE(ego != nullptr);
    CHECK(ego->world_previous == fixture.ego_instance(before)->world_previous);
    CHECK(ego->world_current == fixture.ego_instance(before)->world_current);
}

TEST_CASE("simulation loop: the time fields of ticks and of Part A without a scene source", "[visualizer][unit]")
{
    SECTION("no scene source: step 0, and an anchor at rest")
    {
        loop_fixture fixture{ false };
        const scene::snapshot_reference reference = fixture.snapshots.acquire();
        REQUIRE(reference);
        CHECK(reference->step == 0);
        CHECK(reference->time_current == avis::simulation::simulation_duration{ 0 });
        CHECK(reference->anchor.t_base == avis::simulation::simulation_duration{ 0 });
        CHECK(reference->anchor.wall_base == start_time);
        CHECK(reference->anchor.rate == 0.0);
    }

    SECTION("demo ticks: a step committed while running, at speed 1")
    {
        loop_fixture fixture{ true };
        fixture.advance_to(start_time);
        fixture.advance_to(start_time + 130ms); // tick 2, late: the next one is 100 ms after this
        const scene::snapshot_reference reference = fixture.snapshots.acquire();
        REQUIRE(reference);
        CHECK(reference->step == 2);
        CHECK(reference->time_previous == 100ms);
        CHECK(reference->time_current == 200ms);
        CHECK(reference->anchor.t_base == 100ms);
        CHECK(reference->anchor.wall_base == start_time + 130ms);
        CHECK(reference->anchor.rate == 1.0);
        CHECK(fixture.loop->next_due() <= start_time + 230ms);
    }
}

TEST_CASE("simulation loop: Reset leaves only the helpers", "[visualizer][unit]")
{
    loop_fixture fixture{ true };
    fixture.advance_to(start_time);
    scene::scene& world = fixture.loop->scene();
    REQUIRE(driver_entities(world) >= 9);

    // A helper outside its owner's subtree, as trails are (08 §8.2)
    const std::optional<scene::entity> ego = world.find(demo_scene::ego_vehicle_id);
    REQUIRE(ego);
    std::optional<scene::entity> helpers;
    for (const auto [entity, who] : world.registry().view<identity>().each())
    {
        if (who.name == simulation_loop::helpers_name)
        {
            helpers = entity;
        }
    }
    REQUIRE(helpers);
    const result<scene::entity> trail = world.create_entity({ .name = "ego trail", .parent = *helpers });
    REQUIRE(trail);
    world.registry().emplace<represents>(*trail, represents{ .owner = *ego });

    fixture.handle(simulation::reset_scene{});
    fixture.advance_to(start_time + simulation_loop::republish_interval);

    CHECK(driver_entities(world) == 0);
    CHECK_FALSE(world.alive(*trail));
    REQUIRE(world.alive(*helpers));
    for (const auto [entity, who] : world.registry().view<identity>().each())
    {
        CHECK(who.category == scene::entity_category::helper);
    }
    const scene::snapshot_reference reference = fixture.snapshots.acquire();
    REQUIRE(reference);
    CHECK(reference->instances.empty()); // the fake representation drew only Driver entities
    CHECK(fixture.sink.count(scene::tree_change_kind::removed, demo_scene::ego_vehicle_id) == 1);

    // The demo scene comes back on its next tick
    fixture.advance_to(start_time + demo_scene::tick_interval);
    CHECK(world.find(demo_scene::ego_vehicle_id));
}

TEST_CASE("simulation loop: select and clear move the selected flag", "[visualizer][unit]")
{
    loop_fixture fixture{ true };
    fixture.advance_to(start_time);

    const auto selected_ids = [&]
    {
        std::set<scene::object_id> ids;
        const scene::snapshot_reference reference = fixture.snapshots.acquire();
        REQUIRE(reference);
        for (const scene::render_instance& instance : reference->instances)
        {
            if (static_cast<bool>(instance.flags & scene::instance_flags::selected))
            {
                ids.insert(instance.object_id);
            }
        }
        return ids;
    };
    CHECK(selected_ids().empty());

    // A helper that represents the shadow vehicle, with a mesh: selecting it selects the owner, and both are flagged
    scene::scene& world = fixture.loop->scene();
    const scene::entity shadow = *world.find(demo_scene::shadow_vehicle_id);
    const result<scene::entity> edges = world.create_entity({ .name = "edges", .parent = shadow });
    REQUIRE(edges);
    world.registry().emplace<represents>(*edges, represents{ .owner = shadow });
    world.registry().emplace<mesh>(*edges, world.registry().get<mesh>(shadow));
    world.registry().emplace<material>(*edges, world.registry().get<material>(shadow));
    const scene::object_id edges_id = world.registry().get<identity>(*edges).id;

    fixture.handle(simulation::select_entity{ .id = edges_id });
    CHECK(fixture.loop->presentation().selected == demo_scene::shadow_vehicle_id);
    fixture.advance_to(start_time + simulation_loop::republish_interval);
    CHECK(selected_ids() == std::set{ demo_scene::shadow_vehicle_id, edges_id });

    fixture.handle(simulation::select_entity{ .id = demo_scene::ego_vehicle_id });
    fixture.advance_to(start_time + 2 * simulation_loop::republish_interval);
    CHECK(selected_ids() == std::set{ demo_scene::ego_vehicle_id });

    fixture.handle(simulation::clear_selection{});
    fixture.advance_to(start_time + 3 * simulation_loop::republish_interval);
    CHECK(selected_ids().empty());
    CHECK(fixture.loop->presentation().selected == 0);
}

TEST_CASE("simulation loop: visibility toggles reach the snapshot", "[visualizer][unit]")
{
    loop_fixture fixture{ true };
    fixture.advance_to(start_time);

    const auto hidden_ids = [&]
    {
        std::set<scene::object_id> ids;
        const scene::snapshot_reference reference = fixture.snapshots.acquire();
        REQUIRE(reference);
        for (const scene::render_instance& instance : reference->instances)
        {
            if (!(instance.flags & scene::instance_flags::visible))
            {
                ids.insert(instance.object_id);
            }
        }
        return ids;
    };
    CHECK(hidden_ids().empty());

    fixture.handle(simulation::set_entity_visibility{ .id = demo_scene::ego_vehicle_id, .visible = false });
    fixture.advance_to(start_time + simulation_loop::republish_interval);
    CHECK(hidden_ids() == std::set{ demo_scene::ego_vehicle_id });

    fixture.handle(simulation::set_category_visibility{ .category = scene::entity_category::object, .visible = false });
    fixture.advance_to(start_time + 2 * simulation_loop::republish_interval);
    const std::set<scene::object_id> hidden = hidden_ids();
    CHECK(hidden.contains(demo_scene::ego_vehicle_id));
    CHECK(hidden.size() > demo_scene::box_count / 2); // the boxes
    CHECK_FALSE(hidden.contains(demo_scene::shadow_vehicle_id));

    // New boxes of a hidden category are hidden too
    for (int tick = 1; tick <= 20; ++tick)
    {
        fixture.advance_to(start_time + tick * demo_scene::tick_interval);
    }
    const scene::snapshot_reference reference = fixture.snapshots.acquire();
    REQUIRE(reference);
    for (const scene::render_instance& instance : reference->instances)
    {
        if (instance.object_id >= demo_scene::driver_id(1000))
        {
            CHECK_FALSE(static_cast<bool>(instance.flags & scene::instance_flags::visible));
        }
    }
}

TEST_CASE("simulation loop: what the GUI gets, and how often", "[visualizer][unit]")
{
    loop_fixture fixture{ true };

    SECTION("tree changes after a structural change")
    {
        // The helpers group, at construction
        REQUIRE(fixture.sink.trees.size() == 1);
        CHECK(fixture.sink.trees.front().front().kind == scene::tree_change_kind::added);
        CHECK(fixture.sink.trees.front().front().name == simulation_loop::helpers_name);

        fixture.advance_to(start_time);
        CHECK(fixture.sink.count(scene::tree_change_kind::added, demo_scene::ego_vehicle_id) == 1);

        // A tick that only moves things posts nothing
        const std::size_t batches = fixture.sink.trees.size();
        fixture.advance_to(start_time + demo_scene::tick_interval);
        fixture.advance_to(start_time + 2 * demo_scene::tick_interval);
        for (std::size_t index = batches; index < fixture.sink.trees.size(); ++index)
        {
            for (const scene::tree_change& change : fixture.sink.trees[index])
            {
                CHECK(change.id >= demo_scene::driver_id(1000)); // boxes coming and going
            }
        }

        fixture.handle(simulation::reset_scene{});
        fixture.advance_to(start_time + 2 * demo_scene::tick_interval + simulation_loop::republish_interval);
        CHECK(fixture.sink.count(scene::tree_change_kind::removed, demo_scene::ego_vehicle_id) == 1);
    }

    SECTION("the selected entity at no more than 20 Hz, and the clear")
    {
        fixture.advance_to(start_time);
        fixture.handle(simulation::select_entity{ .id = demo_scene::ego_vehicle_id });

        // One second, the loop running every millisecond, with demo ticks and a presentation command every 5 ms
        for (int millisecond = 1; millisecond <= 1000; ++millisecond)
        {
            if (millisecond % 5 == 0)
            {
                fixture.handle(simulation::set_staleness{ .enabled = millisecond % 10 == 0 });
            }
            fixture.advance_to(start_time + millisecond * 1ms);
        }
        const auto& posts = fixture.sink.selected;
        REQUIRE(posts.size() >= 10);
        CHECK(posts.size() <= 21);
        for (std::size_t index = 1; index < posts.size(); ++index)
        {
            CHECK(posts[index].first - posts[index - 1].first >= simulation_loop::selected_interval);
        }
        for (const auto& [time, entity] : posts)
        {
            REQUIRE(entity);
            CHECK(entity->id == demo_scene::ego_vehicle_id);
            CHECK(entity->name == "ego_vehicle");
        }
        CHECK_FALSE(posts.back().second->components.empty());
        CHECK_FALSE(posts.back().second->properties.empty());

        fixture.handle(simulation::clear_selection{});
        fixture.advance_to(start_time + 1100ms);
        REQUIRE(fixture.sink.selected.back().second == std::nullopt);
        const std::size_t after_clear = fixture.sink.selected.size();
        fixture.advance_to(start_time + 1500ms);
        CHECK(fixture.sink.selected.size() == after_clear); // nothing selected, nothing more posted
    }

    SECTION("statistics at 2 Hz")
    {
        for (int millisecond = 0; millisecond <= 2000; millisecond += 10)
        {
            fixture.advance_to(start_time + millisecond * 1ms);
        }
        const auto& posts = fixture.sink.statistics;
        REQUIRE(posts.size() == 5); // 0, 0.5, 1, 1.5 and 2 s
        for (std::size_t index = 1; index < posts.size(); ++index)
        {
            CHECK(posts[index].first - posts[index - 1].first == simulation_loop::statistics_interval);
        }
        CHECK(posts.back().second.entities > demo_scene::box_count);
        CHECK(posts.back().second.step == fixture.loop->step());
    }

    SECTION("the follow target: the primary vehicle, or the selected vehicle")
    {
        fixture.advance_to(start_time);
        REQUIRE_FALSE(fixture.sink.follows.empty());
        CHECK(fixture.sink.follows.back().id == demo_scene::ego_vehicle_id);
        CHECK(fixture.sink.follows.back().bounds.has_value());

        // Moving and turning don't change the extent: no new post
        const std::size_t posts = fixture.sink.follows.size();
        fixture.advance_to(start_time + demo_scene::tick_interval);
        fixture.advance_to(start_time + 2 * demo_scene::tick_interval);
        CHECK(fixture.sink.follows.size() == posts);

        // A new extent from the Driver does
        scene::scene& world = fixture.loop->scene();
        world.registry().patch<display_shape>(
            *world.find(demo_scene::ego_vehicle_id), [](display_shape& shape) { shape.extent.x() = 6.0f; });
        fixture.advance_to(start_time + 3 * demo_scene::tick_interval);
        REQUIRE(fixture.sink.follows.size() == posts + 1);
        CHECK(fixture.sink.follows.back().id == demo_scene::ego_vehicle_id);

        fixture.handle(simulation::select_entity{ .id = demo_scene::shadow_vehicle_id });
        fixture.advance_to(start_time + 3 * demo_scene::tick_interval + simulation_loop::republish_interval);
        CHECK(fixture.sink.follows.back().id == demo_scene::shadow_vehicle_id);

        // A box isn't a vehicle: back to the primary one
        fixture.handle(simulation::select_entity{ .id = demo_scene::driver_id(1000) });
        fixture.advance_to(start_time + 3 * demo_scene::tick_interval + 2 * simulation_loop::republish_interval);
        CHECK(fixture.sink.follows.back().id == demo_scene::ego_vehicle_id);
    }
}

TEST_CASE("simulation loop: simulation commands wait for Part B", "[visualizer][unit]")
{
    tests::log_capture log{ "visualizer" };
    loop_fixture fixture{ false };
    const std::uint64_t publishes = fixture.loop->publishes();
    fixture.handle(simulation::start_simulation{});
    fixture.handle(simulation::set_speed{ .scale = 2.0 });
    fixture.advance_to(start_time + 100ms);
    CHECK(fixture.loop->publishes() == publishes);
    CHECK(log.contains("'start' needs a connected Driver"));
    CHECK(log.contains("'set speed' needs a connected Driver"));
}

TEST_CASE("demo scene: 1,000 ticks with no debug check, and a bounded entity count", "[visualizer][unit]")
{
    tests::recorded_assertions assertions;
    tests::log_capture scene_log{ "scene", 64, spdlog::level::err };
    tests::log_capture visualizer_log{ "visualizer", 64, spdlog::level::err };

    loop_fixture fixture{ true };
    std::size_t most_entities = 0;
    std::size_t most_driver_entities = 0;
    for (int tick = 0; tick < 1000; ++tick)
    {
        fixture.advance_to(start_time + tick * demo_scene::tick_interval);
        if (tick == 500)
        {
            fixture.handle(simulation::select_entity{ .id = demo_scene::ego_vehicle_id });
        }
        most_driver_entities = std::max(most_driver_entities, driver_entities(fixture.loop->scene()));
        most_entities = std::max<std::size_t>(most_entities, fixture.loop->scene().statistics().entities);
    }
    CHECK(fixture.loop->step() == 1000);
    CHECK(assertions.count() == 0);
    CHECK(scene_log.lines().empty());
    CHECK(visualizer_log.lines().empty());

    // 9 fixed entities and at most 30 boxes, plus the helpers group
    CHECK(most_driver_entities <= 9 + demo_scene::box_count);
    CHECK(most_entities <= 10 + demo_scene::box_count);

    // Boxes came and went
    std::size_t boxes_removed = 0;
    for (const auto& batch : fixture.sink.trees)
    {
        boxes_removed += static_cast<std::size_t>(std::ranges::count_if(
            batch,
            [](const scene::tree_change& change)
            { return change.kind == scene::tree_change_kind::removed && change.id >= demo_scene::driver_id(1000); }));
    }
    CHECK(boxes_removed > 100);

    // Ids use the Driver source byte 1, never the helper range
    for (const auto [entity, who, from_driver] :
         fixture.loop->scene().registry().view<identity, driver_entity>().each())
    {
        CHECK((who.id >> 56) == 1);
        CHECK_FALSE(scene::is_helper(who.id));
    }

    // The scene source's components, stamped
    scene::scene& world = fixture.loop->scene();
    const scene::entity velodyne = *world.find(demo_scene::driver_id(4));
    REQUIRE(world.registry().all_of<lidar_points>(velodyne));
    CHECK(world.registry().get<lidar_points>(velodyne).sweeps.size() == 3);
    const update_stamp& stamp = world.registry().get<update_stamp>(velodyne);
    REQUIRE(stamp.geometry);
    CHECK(stamp.geometry->step == 1000);
    const update_stamp& ego_stamp = world.registry().get<update_stamp>(*world.find(demo_scene::ego_vehicle_id));
    REQUIRE(ego_stamp.pose);
    CHECK(ego_stamp.pose->step == 1000);
    CHECK(world.registry().get<vehicle>(*world.find(demo_scene::ego_vehicle_id)).primary);
}

TEST_CASE("simulation thread: runs the loop on a pool thread", "[visualizer][unit]")
{
    // Counts what arrives, from the simulation thread
    class counting_sink final : public simulation::gui_sink
    {
    public:
        void tree_changed(std::vector<scene::tree_change>) override
        {
            note([this] { ++trees; });
        }

        void selected_entity_changed(std::optional<scene::reflected_entity> entity) override
        {
            note([&] { last_selected = entity ? entity->id : 0; });
        }

        void statistics_changed(const scene::scene_statistics&) override
        {
            note([this] { ++statistics; });
        }

        void follow_target_changed(const simulation::follow_target&) override {}

        // Waits up to two seconds for `done`
        bool wait_for(const std::function<bool()>& done)
        {
            std::unique_lock lock{ mutex };
            return changed.wait_for(lock, 2s, [&] { return done(); });
        }

        std::mutex mutex;
        std::condition_variable changed;
        std::size_t trees = 0;
        std::size_t statistics = 0;
        scene::object_id last_selected = 0;

    private:
        void note(const std::function<void()>& change)
        {
            {
                const std::lock_guard lock{ mutex };
                change();
            }
            changed.notify_all();
        }
    };

    threading::thread_pool pool{ { .capacity = 1 } };
    scene::asset_cache assets;
    scene::snapshot_buffer snapshots;
    counting_sink sink;

    simulation::simulation_settings settings;
    settings.demo_scene = true;
    result<std::unique_ptr<simulation::simulation_thread>> started =
        simulation::simulation_thread::start(pool, assets, snapshots, sink, settings);
    REQUIRE(started);
    std::unique_ptr<simulation::simulation_thread> thread = std::move(*started);

    // The demo ticks, and the statistics come at 2 Hz
    CHECK(sink.wait_for([&] { return sink.statistics >= 2 && sink.trees >= 2; }));
    {
        const scene::snapshot_reference reference = snapshots.acquire();
        REQUIRE(reference);
        CHECK(reference->step >= 3);
    }

    // A command from this thread reaches the loop
    thread->post(simulation::select_entity{ .id = demo_scene::ego_vehicle_id });
    CHECK(sink.wait_for([&] { return sink.last_selected == demo_scene::ego_vehicle_id; }));

    // A second thread doesn't fit the pool
    CHECK_FALSE(simulation::simulation_thread::start(pool, assets, snapshots, sink, settings).has_value());

    thread.reset(); // stops and joins
    CHECK(pool.running() == 0);
}
