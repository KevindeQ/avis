// Component reflection: the tables, their units, the readers and the order of reflect_components
// (docs/tasks/10-scene-management.md §10.8)

#include "avis/scene/components/spatial.h"
#include "avis/scene/reflection.h"

#include <entt/entity/registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace avis;
using namespace avis::scene;
using namespace avis::scene::components;

static_assert(reflection<vehicle>::fields.size() == 7);
static_assert(reflection<vehicle>::name == "vehicle");
static_assert(reflection<detail::geometry_summary>::name == "geometry");
static_assert(!reflected_type<properties>);
static_assert(!reflected_type<node_reference>);
static_assert(!reflected_type<belief_geometry>);

namespace
{
    // The units 08 uses
    constexpr std::array known_units = { std::string_view{ "" },    std::string_view{ "m" },
                                         std::string_view{ "m/s" }, std::string_view{ "m/s²" },
                                         std::string_view{ "rad" }, std::string_view{ "rad/s" },
                                         std::string_view{ "s" },   std::string_view{ "lx" },
                                         std::string_view{ "cd" } };

    template<typename Component>
    void check_table()
    {
        INFO(reflection<Component>::name);
        CHECK_FALSE(reflection<Component>::name.empty());
        std::set<std::string_view> names;
        for (const field_description& field : reflection<Component>::fields)
        {
            INFO(field.name);
            CHECK_FALSE(field.name.empty());
            CHECK(names.insert(field.name).second);
            CHECK(std::ranges::find(known_units, field.unit) != known_units.end());
            CHECK(field.read != nullptr);
        }
    }

    template<typename... Components>
    void check_tables(detail::type_list<Components...>)
    {
        (check_table<Components>(), ...);

        // The component names are unique too
        std::set<std::string_view> names;
        (names.insert(reflection<Components>::name), ...);
        CHECK(names.size() == sizeof...(Components));
    }

    const reflected_field& field(const reflected_component& component, const std::string_view name)
    {
        const auto found = std::ranges::find(component.fields, name, &reflected_field::name);
        REQUIRE(found != component.fields.end());
        return *found;
    }
} // namespace

TEST_CASE("reflection tables have unique field names and known units", "[scene][unit]")
{
    check_tables(detail::reflected_components{});
}

TEST_CASE("the vehicle readers return the stored values", "[scene][unit]")
{
    vehicle car;
    car.primary = true;
    car.state.time = std::chrono::milliseconds{ 1500 };
    car.state.speed = 8.5f;
    car.state.steering_angle = -0.25f;
    car.state.linear_velocity = { 8.5f, 0.5f, 0.0f };
    car.state.angular_velocity = { 0.0f, 0.0f, 0.125f };
    car.state.acceleration = { 1.0f, -2.0f, 0.0f };

    const reflected_component reflected = reflect_component(car);
    CHECK(reflected.name == "vehicle");
    REQUIRE(reflected.fields.size() == reflection<vehicle>::fields.size());

    // In table order
    for (std::size_t at = 0; at < reflected.fields.size(); ++at)
    {
        CHECK(reflected.fields[at].name == reflection<vehicle>::fields[at].name);
        CHECK(reflected.fields[at].unit == reflection<vehicle>::fields[at].unit);
    }

    CHECK(std::get<bool>(field(reflected, "primary").value));
    CHECK(
        std::get<simulation::simulation_duration>(field(reflected, "time").value) == std::chrono::milliseconds{ 1500 });
    CHECK(std::get<double>(field(reflected, "speed").value) == 8.5);
    CHECK(field(reflected, "speed").unit == "m/s");
    CHECK(std::get<double>(field(reflected, "steering_angle").value) == -0.25);
    CHECK(std::get<Eigen::Vector3d>(field(reflected, "linear_velocity").value) == Eigen::Vector3d{ 8.5, 0.5, 0.0 });
    CHECK(std::get<Eigen::Vector3d>(field(reflected, "angular_velocity").value) == Eigen::Vector3d{ 0.0, 0.0, 0.125 });
    CHECK(std::get<Eigen::Vector3d>(field(reflected, "acceleration").value) == Eigen::Vector3d{ 1.0, -2.0, 0.0 });
    CHECK(field(reflected, "acceleration").unit == "m/s²");
}

TEST_CASE("readers convert enums, flags, handles and counts", "[scene][unit]")
{
    SECTION("identity")
    {
        const identity named{ .id = 1042, .name = "car 1042", .category = entity_category::object };
        const reflected_component reflected = reflect_component(named);
        CHECK(std::get<std::uint64_t>(field(reflected, "id").value) == 1042);
        CHECK(std::get<std::string>(field(reflected, "name").value) == "car 1042");
        CHECK(std::get<std::string>(field(reflected, "category").value) == "object");
    }

    SECTION("render_options flags become booleans")
    {
        render_options options;
        options.layer = draw_layer::overlay;
        options.layer_priority = 3;
        options.flags = instance_flags::visible | instance_flags::screen_aligned;
        const reflected_component reflected = reflect_component(options);
        CHECK(std::get<std::string>(field(reflected, "layer").value) == "overlay");
        CHECK(std::get<std::uint64_t>(field(reflected, "layer_priority").value) == 3);
        CHECK(std::get<bool>(field(reflected, "visible").value));
        CHECK(std::get<bool>(field(reflected, "screen_aligned").value));
        CHECK_FALSE(std::get<bool>(field(reflected, "casts_shadows").value));
        CHECK_FALSE(std::get<bool>(field(reflected, "pickable").value));
    }

    SECTION("mesh and material as handles and counts")
    {
        const mesh drawn{ .asset = { .index = 4, .generation = 2 }, .primitive_first = 1, .primitive_count = 3 };
        const reflected_component reflected_mesh = reflect_component(drawn);
        CHECK(std::get<std::uint64_t>(field(reflected_mesh, "asset_index").value) == 4);
        CHECK(std::get<std::uint64_t>(field(reflected_mesh, "asset_generation").value) == 2);
        CHECK(std::get<std::uint64_t>(field(reflected_mesh, "primitive_first").value) == 1);
        CHECK(std::get<std::uint64_t>(field(reflected_mesh, "primitive_count").value) == 3);

        material materials;
        materials.materials.resize(2);
        CHECK(std::get<std::uint64_t>(field(reflect_component(materials), "material_count").value) == 2);
    }

    SECTION("a light's intensity is in lux or candela by kind")
    {
        light sun{ .kind = light_kind::directional, .intensity = 100000.0f };
        CHECK(std::get<double>(field(reflect_component(sun), "illuminance").value) == 100000.0);
        CHECK(std::get<double>(field(reflect_component(sun), "luminous_intensity").value) == 0.0);

        light lamp{ .kind = light_kind::spot, .intensity = 800.0f };
        CHECK(std::get<std::string>(field(reflect_component(lamp), "kind").value) == "spot");
        CHECK(std::get<double>(field(reflect_component(lamp), "illuminance").value) == 0.0);
        CHECK(std::get<double>(field(reflect_component(lamp), "luminous_intensity").value) == 800.0);
    }

    SECTION("trail settings and the sample count")
    {
        trail path;
        path.settings.duration_seconds = 5.0f;
        path.samples.resize(3);
        const reflected_component reflected = reflect_component(path);
        CHECK(std::get<double>(field(reflected, "duration").value) == 5.0);
        CHECK(std::get<std::uint64_t>(field(reflected, "sample_count").value) == 3);
    }

    SECTION("update_stamp's created and latest")
    {
        update_stamp stamp;
        stamp.created = { .step = 10, .time = std::chrono::seconds{ 1 } };
        stamp.pose = update_time{ .step = 25, .time = std::chrono::milliseconds{ 2500 } };
        const reflected_component reflected = reflect_component(stamp);
        CHECK(std::get<std::uint64_t>(field(reflected, "created_step").value) == 10);
        CHECK(std::get<std::uint64_t>(field(reflected, "latest_step").value) == 25);
        CHECK(
            std::get<simulation::simulation_duration>(field(reflected, "latest_time").value) ==
            std::chrono::milliseconds{ 2500 });
    }

    SECTION("enum names are distinct")
    {
        std::set<std::string_view> names;
        for (const auto kind : { sensor_kind::camera, sensor_kind::lidar, sensor_kind::gnss_imu })
        {
            CHECK(names.insert(to_string(kind)).second);
        }
        names.clear();
        for (int category = 0; category < static_cast<int>(entity_category_count); ++category)
        {
            CHECK(names.insert(to_string(static_cast<entity_category>(category))).second);
        }
    }
}

TEST_CASE("reflect_components follows the header order, not the emplace order", "[scene][unit]")
{
    entt::registry registry;
    const entity car = registry.create();

    // Emplaced in reverse header order, with components that aren't reflected in between
    registry.emplace<update_stamp>(car);
    registry.emplace<trail>(car);
    registry.emplace<label>(car);
    registry.emplace<properties>(car).set("perception.speed", 8.333, "m/s");
    registry.emplace<display_shape>(car);
    registry.emplace<driver_entity>(car);
    registry.emplace<sensor>(car);
    registry.emplace<vehicle>(car);
    registry.emplace<light>(car);
    registry.emplace<visibility>(car);
    registry.emplace<render_options>(car);
    registry.emplace<material>(car);
    registry.emplace<mesh>(car);
    registry.emplace<previous_world>(car);
    registry.emplace<identity>(car, identity{ .id = 7, .name = "car", .category = entity_category::vehicle });

    std::vector<reflected_component> reflected;
    detail::reflect_components(registry, car, reflected);

    std::vector<std::string_view> names;
    for (const reflected_component& component : reflected)
    {
        names.push_back(component.name);
    }
    const std::vector<std::string_view> expected = { "identity",      "mesh",          "material", "render_options",
                                                     "visibility",    "light",         "vehicle",  "sensor",
                                                     "driver_entity", "display_shape", "label",    "trail",
                                                     "update_stamp" };
    CHECK(names == expected);

    SECTION("geometry appears with belief_geometry or lidar_points")
    {
        geometry_chunk chunk;
        chunk.positions.resize(4);
        chunk.indices.resize(6);
        registry.emplace<belief_geometry>(car).replace(std::move(chunk));

        lidar_sweep sweep;
        sweep.positions.resize(10);
        sweep.intensity.resize(10);
        registry.emplace<lidar_points>(car).push(std::move(sweep));

        reflected.clear();
        detail::reflect_components(registry, car, reflected);
        REQUIRE(reflected.size() == expected.size() + 1);
        const reflected_component& geometry = reflected[10]; // after display_shape, before label
        REQUIRE(geometry.name == "geometry");
        CHECK(std::get<std::uint64_t>(field(geometry, "chunk_count").value) == 1);
        CHECK(std::get<std::uint64_t>(field(geometry, "vertex_count").value) == 4);
        CHECK(std::get<std::uint64_t>(field(geometry, "sweep_count").value) == 1);
        CHECK(std::get<std::uint64_t>(field(geometry, "point_count").value) == 10);
        CHECK(
            std::get<std::uint64_t>(field(geometry, "byte_count").value) ==
            4 * sizeof(Eigen::Vector3f) + 6 * sizeof(std::uint32_t) + 10 * sizeof(Eigen::Vector3f) + 10);

        // lidar_points alone is enough
        registry.remove<belief_geometry>(car);
        reflected.clear();
        detail::reflect_components(registry, car, reflected);
        REQUIRE(reflected.size() == expected.size() + 1);
        CHECK(std::get<std::uint64_t>(field(reflected[10], "chunk_count").value) == 0);
    }

    SECTION("an entity with no reflected component gives nothing")
    {
        const entity bare = registry.create();
        registry.emplace<node_reference>(bare);
        reflected.clear();
        detail::reflect_components(registry, bare, reflected);
        CHECK(reflected.empty());
    }
}
