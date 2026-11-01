// The scene components: storage requirements, properties, belief geometry, lidar sweeps, update stamps and the shared
// flags (docs/tasks/10-scene-management.md §10.7)

#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"
#include "avis/scene/components/sensor.h"
#include "avis/scene/components/spatial.h"
#include "avis/scene/components/vehicle.h"
#include "avis/scene/components/viewing.h"
#include "avis/scene/flags.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

using namespace avis;
using namespace avis::scene::components;

namespace
{
    // Every component, in the order of the headers in 08 §9.2
    template<typename... Components>
    struct component_list
    {};

    using every_component = component_list<
        identity,
        represents,
        node_reference,
        previous_world,
        mesh,
        material,
        render_options,
        visibility,
        light,
        vehicle,
        sensor,
        driver_entity,
        display_shape,
        properties,
        belief_geometry,
        lidar_points,
        label,
        trail,
        update_stamp>;

    // MSVC's std::deque move constructor isn't noexcept (it allocates its iterator proxy), and 08 §8.2 makes
    // trail::samples a std::deque. EnTT only needs movable components.
    template<typename Component>
    constexpr bool nothrow_move_expected = !std::is_same_v<Component, trail>;

    template<typename Component>
    constexpr bool storable() noexcept
    {
        static_assert(std::is_default_constructible_v<Component>);
        static_assert(std::is_move_constructible_v<Component>);
        static_assert(std::is_move_assignable_v<Component>);
        static_assert(!nothrow_move_expected<Component> || std::is_nothrow_move_constructible_v<Component>);
        static_assert(!std::is_pointer_v<Component>);
        return true;
    }

    template<typename... Components>
    constexpr bool all_storable(component_list<Components...>) noexcept
    {
        return (storable<Components>() && ...);
    }

    static_assert(all_storable(every_component{}));

    // The reference for properties: insertion order, found by a linear search
    struct reference_properties
    {
        std::vector<property_entry> entries;

        property_entry* find(const std::string& key)
        {
            const auto found =
                std::ranges::find_if(entries, [&](const property_entry& entry) { return entry.key == key; });
            return found == entries.end() ? nullptr : &*found;
        }
    };
} // namespace

TEST_CASE("component defaults follow 08", "[scene][unit]")
{
    const render_options options;
    CHECK(options.layer == scene::draw_layer::world);
    CHECK(options.flags == scene::default_instance_flags);
    CHECK(options.maximum_view_distance == 0.0f);

    const represents helper;
    CHECK(helper.owner == entt::null);

    const mesh drawn;
    CHECK_FALSE(drawn.asset.valid());
    CHECK(drawn.primitive_first == 0);
    CHECK(drawn.primitive_count == mesh::all_primitives);

    const visibility shown;
    CHECK(shown.visible);
    CHECK(shown.layers == scene::all_render_layers);

    const driver_entity marker;
    CHECK(marker.flags.empty());

    CHECK(previous_world{}.world.isApprox(Eigen::Affine3f::Identity()));
}

TEST_CASE("properties keep insertion order and find through the index", "[scene][unit]")
{
    SECTION("set, replace and remove")
    {
        properties values;
        values.set("perception.speed", 8.333, "m/s");
        values.set("perception.classification", std::string{ "car" });
        values.set("estimate.dimensions", Eigen::Vector3d{ 4.4, 1.8, 1.5 }, "m");
        values.set("perception.tracked", true);

        // Replacing keeps the position
        values.set("perception.speed", 9.0, "m/s");
        REQUIRE(values.size() == 4);
        CHECK(values.entries()[0].key == "perception.speed");
        CHECK(std::get<double>(values.entries()[0].value) == 9.0);

        REQUIRE(values.remove("perception.classification"));
        CHECK_FALSE(values.remove("perception.classification"));
        REQUIRE(values.size() == 3);
        CHECK(values.entries()[0].key == "perception.speed");
        CHECK(values.entries()[1].key == "estimate.dimensions");
        CHECK(values.entries()[2].key == "perception.tracked");

        const property_entry* dimensions = values.find("estimate.dimensions");
        REQUIRE(dimensions != nullptr);
        CHECK(dimensions->unit == "m");
        CHECK(std::get<Eigen::Vector3d>(dimensions->value).isApprox(Eigen::Vector3d{ 4.4, 1.8, 1.5 }));
        CHECK(values.find("perception.classification") == nullptr);
        CHECK(values.find("perception") == nullptr);

        values.clear();
        CHECK(values.empty());
        CHECK(values.find("perception.speed") == nullptr);
    }

    SECTION("1,000 random operations against a linear search")
    {
        std::mt19937 random{ 1007 };
        std::vector<std::string> keys;
        for (int key = 0; key < 24; ++key)
        {
            keys.push_back(std::format("group{}.key{}", key % 5, (key * 7) % 24));
        }
        std::uniform_int_distribution<std::size_t> pick{ 0, keys.size() - 1 };
        std::uniform_int_distribution<int> operation{ 0, 2 };

        properties values;
        reference_properties reference;
        for (int step = 0; step < 1000; ++step)
        {
            const std::string& key = keys[pick(random)];
            if (operation(random) < 2)
            {
                const auto value = static_cast<std::int64_t>(step);
                const std::string unit = step % 2 == 0 ? "m" : "";
                values.set(key, value, unit);
                if (property_entry* existing = reference.find(key))
                {
                    existing->value = value;
                    existing->unit = unit;
                }
                else
                {
                    reference.entries.push_back({ .key = key, .value = value, .unit = unit });
                }
            }
            else
            {
                const bool existed = reference.find(key) != nullptr;
                CHECK(values.remove(key) == existed);
                std::erase_if(reference.entries, [&](const property_entry& entry) { return entry.key == key; });
            }

            REQUIRE(values.entries() == reference.entries);
            for (const std::string& probe : keys)
            {
                const property_entry* found = values.find(probe);
                const property_entry* expected = reference.find(probe);
                REQUIRE((found == nullptr) == (expected == nullptr));
                if (found != nullptr)
                {
                    REQUIRE(*found == *expected);
                }
            }
        }
    }
}

TEST_CASE("belief_geometry keeps its chunks by id", "[scene][unit]")
{
    belief_geometry geometry;
    for (const std::uint32_t id : { 7u, 2u, 5u })
    {
        geometry_chunk chunk;
        chunk.id = id;
        chunk.positions.assign(id, Eigen::Vector3f::Zero());
        geometry.replace(std::move(chunk));
    }
    REQUIRE(geometry.chunks().size() == 3);
    CHECK(geometry.chunks()[0].id == 2);
    CHECK(geometry.chunks()[1].id == 5);
    CHECK(geometry.chunks()[2].id == 7);

    geometry_chunk replacement;
    replacement.id = 5;
    replacement.topology = scene::components::geometry_topology::points;
    geometry.replace(std::move(replacement));
    REQUIRE(geometry.chunks().size() == 3);
    REQUIRE(geometry.find(5) != nullptr);
    CHECK(geometry.find(5)->topology == geometry_topology::points);
    CHECK(geometry.find(5)->positions.empty());

    CHECK(geometry.remove(2));
    CHECK_FALSE(geometry.remove(2));
    CHECK(geometry.find(2) == nullptr);
    CHECK(geometry.chunks().size() == 2);
}

TEST_CASE("lidar_points keeps the last retention sweeps", "[scene][unit]")
{
    const auto sweep = [](const int at)
    {
        lidar_sweep made;
        made.capture_time = std::chrono::milliseconds{ 100 * at };
        return made;
    };

    lidar_points points;
    points.push(sweep(1));
    points.push(sweep(2));
    REQUIRE(points.sweeps.size() == 1); // retention 1: each sweep replaces the previous one
    CHECK(points.sweeps[0].capture_time == std::chrono::milliseconds{ 200 });

    points.retention = 3;
    for (int at = 3; at <= 6; ++at)
    {
        points.push(sweep(at));
    }
    REQUIRE(points.sweeps.size() == 3);
    CHECK(points.sweeps.front().capture_time == std::chrono::milliseconds{ 400 });
    CHECK(points.sweeps.back().capture_time == std::chrono::milliseconds{ 600 });

    // A lower retention drops the surplus on the next sweep
    points.retention = 0;
    points.push(sweep(7));
    REQUIRE(points.sweeps.size() == 1);
    CHECK(points.sweeps[0].capture_time == std::chrono::milliseconds{ 700 });
}

TEST_CASE("update_stamp::latest picks the newest change by step", "[scene][unit]")
{
    using std::chrono::milliseconds;

    update_stamp stamp;
    stamp.created = { .step = 10, .time = milliseconds{ 1000 } };
    CHECK(stamp.latest() == stamp.created);

    stamp.properties = update_time{ .step = 30, .time = milliseconds{ 3000 } };
    stamp.pose = update_time{ .step = 20, .time = milliseconds{ 2000 } };
    CHECK(stamp.latest() == *stamp.properties);

    stamp.geometry = update_time{ .step = 40, .time = milliseconds{ 4000 } };
    CHECK(stamp.latest() == *stamp.geometry);

    stamp.pose = update_time{ .step = 50, .time = milliseconds{ 5000 } };
    CHECK(stamp.latest() == *stamp.pose);

    // On a tie, the earlier field in the order created, pose, properties, geometry
    update_stamp tied;
    tied.created = { .step = 5, .time = milliseconds{ 1 } };
    tied.geometry = update_time{ .step = 5, .time = milliseconds{ 2 } };
    CHECK(tied.latest() == tied.created);
}

TEST_CASE("instance and driver entity flags combine and test", "[scene][unit]")
{
    using scene::instance_flags;

    avis::flags<instance_flags> flags = instance_flags::visible | instance_flags::screen_aligned;
    CHECK(static_cast<bool>(flags & instance_flags::visible));
    CHECK(static_cast<bool>(flags & instance_flags::screen_aligned));
    CHECK_FALSE(static_cast<bool>(flags & instance_flags::pickable));

    flags |= instance_flags::selected;
    CHECK(flags.count(instance_flags::selected) == 1);
    flags.erase(instance_flags::visible);
    CHECK_FALSE(static_cast<bool>(flags & instance_flags::visible));
    CHECK(flags.size() == 2);

    CHECK(static_cast<bool>(scene::default_instance_flags & instance_flags::casts_shadows));
    CHECK_FALSE(static_cast<bool>(scene::default_instance_flags & instance_flags::selected));
    CHECK_FALSE(static_cast<bool>(scene::default_instance_flags & instance_flags::screen_aligned));

    driver_entity marker;
    marker.flags |= scene::driver_entity_flags::primary;
    CHECK(static_cast<bool>(marker.flags & scene::driver_entity_flags::primary));
    CHECK_FALSE(static_cast<bool>(marker.flags & scene::driver_entity_flags::hidden_by_default));
}
