#include "simulation/demo_scene.h"

#include "avis/log.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/sensor.h"
#include "avis/scene/components/vehicle.h"
#include "avis/scene/components/viewing.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>

namespace avis::visualizer::simulation
{
    namespace
    {
        using namespace avis::scene::components;
        using avis::simulation::simulation_duration;
        using avis::simulation::step_index;
        using scene::entity;
        using scene::entity_category;

        // Ids of the fixed entities (the boxes count up from demo_scene's next_box_)
        constexpr scene::object_id image_00_id = demo_scene::driver_id(2);
        constexpr scene::object_id image_01_id = demo_scene::driver_id(3);
        constexpr scene::object_id velodyne_id = demo_scene::driver_id(4);
        constexpr scene::object_id trajectory_id = demo_scene::driver_id(6);
        constexpr scene::object_id perception_id = demo_scene::driver_id(7);
        constexpr scene::object_id belief_id = demo_scene::driver_id(8);
        constexpr scene::object_id drivable_area_id = demo_scene::driver_id(9);

        constexpr avis::flags<scene::driver_entity_flags> no_flags{ avis::empty };

        constexpr std::array fixed_ids{
            demo_scene::ego_vehicle_id,
            image_00_id,
            image_01_id,
            velodyne_id,
            demo_scene::shadow_vehicle_id,
            trajectory_id,
            perception_id,
            belief_id,
            drivable_area_id,
        };

        // The ego drives a circle around the origin, counter-clockwise (z up)
        constexpr float track_radius = 25.0f; // metres
        constexpr float ego_speed = 8.0f; // m/s
        constexpr float wheelbase = 2.8f; // metres
        constexpr float trajectory_seconds = 3.0f;
        constexpr int trajectory_samples = 31;

        // The synthetic sweep: rings × azimuth steps
        constexpr int lidar_rings = 16;
        constexpr int lidar_azimuths = 256;
        constexpr float lidar_height = 1.7f; // metres above the ground

        float seconds(const simulation_duration time) noexcept
        {
            return std::chrono::duration<float>(time).count();
        }

        scene::transform pose(const Eigen::Vector3f& position, const float yaw)
        {
            scene::transform made;
            made.position = position;
            made.rotation = Eigen::Quaternionf{ Eigen::AngleAxisf{ yaw, Eigen::Vector3f::UnitZ() } };
            return made;
        }

        // The ego's angle on the track at `time`
        float ego_angle(const float time) noexcept
        {
            return time * ego_speed / track_radius;
        }

        // The shadow vehicle runs a little behind the ego and drifts across its path
        float shadow_angle(const float time) noexcept
        {
            return ego_angle(time) - 0.08f;
        }

        float shadow_radius(const float time) noexcept
        {
            return track_radius + 0.9f * std::sin(0.4f * time);
        }

        Eigen::Vector3f on_circle(const float angle, const float radius)
        {
            return { radius * std::cos(angle), radius * std::sin(angle), 0.0f };
        }

        // The tangent's heading of a counter-clockwise circle at `angle`
        float heading(const float angle) noexcept
        {
            return angle + std::numbers::pi_v<float> / 2.0f;
        }

        void stamp_pose(scene::scene& scene, const entity target, const update_time time)
        {
            scene.registry().patch<update_stamp>(target, [&](update_stamp& stamp) { stamp.pose = time; });
        }

        void stamp_properties(scene::scene& scene, const entity target, const update_time time)
        {
            scene.registry().patch<update_stamp>(target, [&](update_stamp& stamp) { stamp.properties = time; });
        }

        void stamp_geometry(scene::scene& scene, const entity target, const update_time time)
        {
            scene.registry().patch<update_stamp>(target, [&](update_stamp& stamp) { stamp.geometry = time; });
        }

        // An entity_update create (08 §3.1): the entity with its identity, driver_entity and the local pose, and an
        // update stamp with every field set. Returns null after logging when the scene refuses it.
        entity create(
            scene::scene& scene,
            const scene::object_id id,
            std::string name,
            const entity_category category,
            const entity parent,
            const scene::transform& local,
            const avis::flags<scene::driver_entity_flags> flags,
            const update_time time)
        {
            const result<entity> created = scene.create_entity(
                { .id = id, .name = std::move(name), .category = category, .parent = parent, .local = local });
            if (!created)
            {
                log::get("visualizer")->error("demo scene: entity {:#x} could not be created: {}", id, created.error());
                return entt::null;
            }
            entt::registry& registry = scene.registry();
            registry.emplace<driver_entity>(*created, category, flags);
            registry.emplace<update_stamp>(*created, time, time, time, time);
            return *created;
        }

        std::optional<entity> find(const scene::scene& scene, const scene::object_id id)
        {
            return scene.find(id);
        }

        display_shape frustum_shape()
        {
            display_shape shape;
            shape.shape = shape_kind::frustum;
            shape.shape_color = scene::color::from_srgb8({ 120, 200, 255, 255 });
            shape.horizontal_fov = 1.4f;
            shape.vertical_fov = 0.5f;
            shape.range = 8.0f;
            return shape;
        }

        // A ring of triangles around the track: the drivable area
        geometry_chunk drivable_area_chunk()
        {
            constexpr int segments = 96;
            constexpr float half_width = 4.0f;
            geometry_chunk chunk;
            chunk.id = 1;
            chunk.topology = geometry_topology::triangles;
            chunk.positions.reserve(2 * segments);
            chunk.normals.reserve(2 * segments);
            for (int segment = 0; segment < segments; ++segment)
            {
                const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(segment) / segments;
                chunk.positions.push_back(on_circle(angle, track_radius - half_width));
                chunk.positions.push_back(on_circle(angle, track_radius + half_width));
                chunk.normals.push_back(Eigen::Vector3f::UnitZ());
                chunk.normals.push_back(Eigen::Vector3f::UnitZ());
            }
            chunk.indices.reserve(6 * segments);
            for (std::uint32_t segment = 0; segment < segments; ++segment)
            {
                const std::uint32_t inner = 2 * segment;
                const std::uint32_t next_inner = 2 * ((segment + 1) % segments);
                for (const std::uint32_t index :
                     { inner, inner + 1, next_inner + 1, inner, next_inner + 1, next_inner })
                {
                    chunk.indices.push_back(index);
                }
            }
            chunk.style.style_color = scene::color::from_srgb8({ 90, 160, 110, 255 });
            chunk.style.opacity = 0.35f;
            chunk.style.lit = false;
            return chunk;
        }

        // The shadow vehicle's next seconds, in its own frame
        geometry_chunk trajectory_chunk(const float time, const Eigen::Affine3f& shadow_world)
        {
            geometry_chunk chunk;
            chunk.id = 1;
            chunk.topology = geometry_topology::line_strip;
            chunk.positions.reserve(trajectory_samples);
            const Eigen::Affine3f to_local = shadow_world.inverse();
            for (int sample = 0; sample < trajectory_samples; ++sample)
            {
                const float ahead = time + trajectory_seconds * static_cast<float>(sample) / (trajectory_samples - 1);
                chunk.positions.push_back(to_local * on_circle(shadow_angle(ahead), shadow_radius(ahead)));
            }
            chunk.style.style_color = scene::color::from_srgb8({ 255, 200, 40, 255 });
            chunk.style.lit = false;
            chunk.style.line_width = 3.0f;
            chunk.style.layer = scene::draw_layer::overlay;
            return chunk;
        }

        // A sweep of the ground and a wall of "buildings" around the sensor, which turns with time
        lidar_sweep make_sweep(const float time, const simulation_duration capture_time, const Eigen::Affine3f& world)
        {
            lidar_sweep sweep;
            sweep.capture_time = capture_time;
            sweep.sensor_position = world.translation().cast<double>();
            sweep.sensor_orientation = Eigen::Quaternionf{ world.rotation() };
            sweep.positions.reserve(lidar_rings * lidar_azimuths);
            sweep.intensity.reserve(lidar_rings * lidar_azimuths);
            sweep.ring.reserve(lidar_rings * lidar_azimuths);
            for (int ring = 0; ring < lidar_rings; ++ring)
            {
                // From 15° down to 2° up
                const float elevation = (-15.0f + 17.0f * static_cast<float>(ring) / (lidar_rings - 1)) *
                                        std::numbers::pi_v<float> / 180.0f;
                for (int step = 0; step < lidar_azimuths; ++step)
                {
                    const float azimuth = 2.0f * std::numbers::pi_v<float> * static_cast<float>(step) / lidar_azimuths;
                    const float wall = 30.0f + 6.0f * std::sin(3.0f * azimuth + 0.2f * time);
                    const float ground = elevation < 0.0f ? lidar_height / std::sin(-elevation) : wall;
                    const float range = std::min(ground, wall);
                    sweep.positions.emplace_back(
                        range * std::cos(elevation) * std::cos(azimuth),
                        range * std::cos(elevation) * std::sin(azimuth),
                        range * std::sin(elevation));
                    sweep.intensity.push_back(static_cast<std::uint8_t>(255.0f * std::clamp(8.0f / range, 0.0f, 1.0f)));
                    sweep.ring.push_back(static_cast<std::uint8_t>(ring));
                }
            }
            return sweep;
        }
    } // namespace

    demo_scene::demo_scene(const std::uint32_t seed) : random_{ seed } {}

    void demo_scene::tick(scene::scene& scene, const step_index step, const simulation_duration time)
    {
        if (!scene.find(ego_vehicle_id))
        {
            create_entities(scene, step, time);
        }
        move_vehicles(scene, step, time);
        update_boxes(scene, step, time);
    }

    void demo_scene::create_entities(scene::scene& scene, const step_index step, const simulation_duration time)
    {
        // Whatever is left of an earlier run (normally nothing: *Reset* destroys every Driver entity)
        for (const scene::object_id id : fixed_ids)
        {
            if (const std::optional<entity> left = find(scene, id))
            {
                static_cast<void>(scene.destroy_entity(*left));
            }
        }
        for (const box& left : boxes_)
        {
            if (const std::optional<entity> found = find(scene, left.id))
            {
                static_cast<void>(scene.destroy_entity(*found));
            }
        }
        boxes_.clear();

        const update_time now{ .step = step, .time = time };
        const float t = seconds(time);
        entt::registry& registry = scene.registry();
        const auto pickable = avis::flags<scene::driver_entity_flags>{ scene::driver_entity_flags::pickable };

        // The ego vehicle and its sensors (08 §4.2)
        const entity ego = create(
            scene,
            ego_vehicle_id,
            "ego_vehicle",
            entity_category::vehicle,
            entt::null,
            pose(on_circle(ego_angle(t), track_radius), heading(ego_angle(t))),
            pickable | scene::driver_entity_flags::primary,
            now);
        if (ego == entt::null)
        {
            return;
        }
        display_shape vehicle_shape;
        vehicle_shape.shape = shape_kind::vehicle;
        vehicle_shape.extent = { 4.6f, 1.9f, 1.5f };
        vehicle_shape.shape_color = scene::color::from_srgb8({ 60, 110, 220, 255 });
        registry.emplace<display_shape>(ego, vehicle_shape);
        registry.emplace<vehicle>(ego, vehicle{ .primary = true });
        properties& ego_properties = registry.emplace<properties>(ego);
        ego_properties.set("estimate.speed", double{ ego_speed }, "m/s");
        ego_properties.set("estimate.track_radius", double{ track_radius }, "m");

        for (const auto& [id, name, offset] :
             { std::tuple{ image_00_id, "image_00", Eigen::Vector3f{ 1.6f, 0.3f, 1.4f } },
               std::tuple{ image_01_id, "image_01", Eigen::Vector3f{ 1.6f, -0.3f, 1.4f } } })
        {
            const entity camera =
                create(scene, id, name, entity_category::sensor, ego, pose(offset, 0.0f), pickable, now);
            if (camera != entt::null)
            {
                registry.emplace<sensor>(camera, sensor{ .kind = sensor_kind::camera });
                registry.emplace<display_shape>(camera, frustum_shape());
            }
        }
        const entity velodyne = create(
            scene,
            velodyne_id,
            "velodyne",
            entity_category::sensor,
            ego,
            pose({ 0.8f, 0.0f, lidar_height }, 0.0f),
            pickable,
            now);
        if (velodyne != entt::null)
        {
            registry.emplace<sensor>(velodyne, sensor{ .kind = sensor_kind::lidar });
            display_shape axes;
            axes.shape = shape_kind::axes;
            axes.extent = Eigen::Vector3f::Constant(0.5f);
            registry.emplace<display_shape>(velodyne, axes);
            registry.emplace<lidar_points>(velodyne, lidar_points{ .retention = 3 });
        }

        // The shadow vehicle and its trajectory
        const entity shadow = create(
            scene,
            shadow_vehicle_id,
            "shadow_vehicle",
            entity_category::vehicle,
            entt::null,
            pose(on_circle(shadow_angle(t), shadow_radius(t)), heading(shadow_angle(t))),
            pickable,
            now);
        if (shadow != entt::null)
        {
            display_shape ghost = vehicle_shape;
            ghost.shape_color = scene::color::from_srgb8({ 230, 230, 230, 255 });
            ghost.opacity = 0.4f;
            registry.emplace<display_shape>(shadow, ghost);
            registry.emplace<vehicle>(shadow);
            registry.emplace<properties>(shadow);
            create(scene, trajectory_id, "trajectory", entity_category::geometry, shadow, {}, no_flags, now);
        }

        // The groups, and the drivable area
        create(scene, perception_id, "perception", entity_category::group, entt::null, {}, no_flags, now);
        const entity belief = create(scene, belief_id, "belief", entity_category::group, entt::null, {}, no_flags, now);
        if (belief != entt::null)
        {
            const entity area =
                create(scene, drivable_area_id, "drivable_area", entity_category::geometry, belief, {}, no_flags, now);
            if (area != entt::null)
            {
                registry.emplace<belief_geometry>(area).replace(drivable_area_chunk());
            }
        }

        for (std::uint32_t index = 0; index < box_count; ++index)
        {
            spawn_box(scene, step, time);
        }
    }

    void demo_scene::move_vehicles(scene::scene& scene, const step_index step, const simulation_duration time)
    {
        const update_time now{ .step = step, .time = time };
        const float t = seconds(time);
        entt::registry& registry = scene.registry();

        if (const std::optional<entity> ego = find(scene, ego_vehicle_id))
        {
            // vehicle_state: the pose and the state (08 §3.1)
            const scene::transform ego_pose = pose(on_circle(ego_angle(t), track_radius), heading(ego_angle(t)));
            scene.set_local(*ego, ego_pose);
            registry.patch<vehicle>(
                *ego,
                [&](vehicle& state)
                {
                    state.state.time = time;
                    state.state.linear_velocity = { ego_speed, 0.0f, 0.0f };
                    state.state.angular_velocity = { 0.0f, 0.0f, ego_speed / track_radius };
                    state.state.acceleration = { 0.0f, ego_speed * ego_speed / track_radius, 0.0f };
                    state.state.speed = ego_speed;
                    state.state.steering_angle = std::atan(wheelbase / track_radius);
                });
            stamp_pose(scene, *ego, now);

            // lidar_sweep, captured at the pose just set
            if (const std::optional<entity> velodyne = find(scene, velodyne_id);
                velodyne && registry.all_of<lidar_points>(*velodyne))
            {
                const Eigen::Affine3f world = ego_pose.to_affine() * scene.local(*velodyne).to_affine();
                registry.patch<lidar_points>(
                    *velodyne, [&](lidar_points& points) { points.push(make_sweep(t, time, world)); });
                stamp_geometry(scene, *velodyne, now);
            }
        }

        if (const std::optional<entity> shadow = find(scene, shadow_vehicle_id))
        {
            const scene::transform shadow_pose =
                pose(on_circle(shadow_angle(t), shadow_radius(t)), heading(shadow_angle(t)));
            scene.set_local(*shadow, shadow_pose);
            registry.patch<vehicle>(
                *shadow,
                [&](vehicle& state)
                {
                    state.state.time = time;
                    state.state.speed = ego_speed;
                    state.state.linear_velocity = { ego_speed, 0.0f, 0.0f };
                });
            stamp_pose(scene, *shadow, now);

            const double offset = shadow_radius(t) - track_radius;
            registry.patch<properties>(
                *shadow,
                [&](properties& values)
                {
                    values.set("control.lateral_offset", offset, "m");
                    values.set("planner.target_speed", double{ ego_speed }, "m/s");
                });
            stamp_properties(scene, *shadow, now);

            if (const std::optional<entity> trajectory = find(scene, trajectory_id))
            {
                static_cast<void>(registry.get_or_emplace<belief_geometry>(*trajectory));
                registry.patch<belief_geometry>(
                    *trajectory,
                    [&](belief_geometry& geometry) { geometry.replace(trajectory_chunk(t, shadow_pose.to_affine())); });
                stamp_geometry(scene, *trajectory, now);
            }
        }
    }

    void demo_scene::update_boxes(scene::scene& scene, const step_index step, const simulation_duration time)
    {
        const update_time now{ .step = step, .time = time };
        const float delta = std::chrono::duration<float>(tick_interval).count();

        std::erase_if(
            boxes_,
            [&](const box& moving)
            {
                const std::optional<entity> found = find(scene, moving.id);
                if (!found)
                {
                    return true; // gone with a *Reset*
                }
                if (time >= moving.expires)
                {
                    static_cast<void>(scene.destroy_entity(*found));
                    return true;
                }
                scene::transform local = scene.local(*found);
                local.position += moving.velocity * delta;
                scene.set_local(*found, local);
                stamp_pose(scene, *found, now);
                return false;
            });

        // Up to two new boxes a tick, so they keep coming and going
        for (int spawned = 0; spawned < 2 && boxes_.size() < box_count; ++spawned)
        {
            spawn_box(scene, step, time);
        }
    }

    void demo_scene::spawn_box(scene::scene& scene, const step_index step, const simulation_duration time)
    {
        const std::optional<entity> perception = find(scene, perception_id);
        if (!perception)
        {
            return;
        }

        struct box_class
        {
            const char* name;
            Eigen::Vector3f extent;
            std::array<std::uint8_t, 4> colour;
            float speed;
        };
        static const std::array<box_class, 3> classes{ {
            { "car", { 4.4f, 1.8f, 1.5f }, { 230, 70, 60, 255 }, 9.0f },
            { "pedestrian", { 0.6f, 0.6f, 1.8f }, { 240, 200, 40, 255 }, 1.4f },
            { "cyclist", { 1.8f, 0.6f, 1.7f }, { 70, 200, 90, 255 }, 5.0f },
        } };

        std::uniform_int_distribution<std::size_t> pick_class{ 0, classes.size() - 1 };
        std::uniform_real_distribution<float> unit{ 0.0f, 1.0f };
        const box_class& kind = classes[pick_class(random_)];
        const float angle = 2.0f * std::numbers::pi_v<float> * unit(random_);
        const float radius = track_radius + 12.0f * (unit(random_) - 0.5f);
        const float direction = 2.0f * std::numbers::pi_v<float> * unit(random_);
        const float speed = kind.speed * (0.5f + unit(random_));
        const auto lifetime = std::chrono::duration_cast<simulation_duration>(
            std::chrono::duration<float>{ 2.0f + 6.0f * unit(random_) });

        const scene::object_id id = driver_id(next_box_);
        const update_time now{ .step = step, .time = time };
        const entity created = create(
            scene,
            id,
            std::format("{} {}", kind.name, next_box_),
            entity_category::object,
            *perception,
            pose(on_circle(angle, radius) + Eigen::Vector3f{ 0.0f, 0.0f, kind.extent.z() / 2.0f }, direction),
            avis::flags<scene::driver_entity_flags>{ scene::driver_entity_flags::pickable },
            now);
        ++next_box_;
        if (created == entt::null)
        {
            return;
        }

        entt::registry& registry = scene.registry();
        display_shape shape;
        shape.shape = shape_kind::box;
        shape.extent = kind.extent;
        shape.shape_color = scene::color::from_srgb8(kind.colour);
        shape.opacity = 0.5f;
        registry.emplace<display_shape>(created, shape);
        properties& values = registry.emplace<properties>(created);
        values.set("perception.classification", std::string{ kind.name });
        values.set("perception.speed", double{ speed }, "m/s");
        values.set("perception.confidence", 0.5 + 0.5 * unit(random_));

        boxes_.push_back(
            box{
                .id = id,
                .velocity = speed * Eigen::Vector3f{ std::cos(direction), std::sin(direction), 0.0f },
                .expires = time + lifetime,
            });
    }
} // namespace avis::visualizer::simulation
