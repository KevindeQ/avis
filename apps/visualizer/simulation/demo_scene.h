#ifndef AVIS_VISUALIZER_SIMULATION_DEMO_SCENE_H
#define AVIS_VISUALIZER_SIMULATION_DEMO_SCENE_H

// A built-in scene source that stands in for the Driver (docs/tasks/12-visualizer.md §12.6): `--demo-scene`, the
// tests, and offline UI work. Each tick it writes the components the scene stream writes in Part B (08 §3.1), through
// the same scene calls, with update stamps:
// - ego_vehicle, the primary vehicle, driving a circle, with two cameras (image_00, image_01) and a lidar (velodyne)
//   that captures a synthetic sweep every tick
// - shadow_vehicle, a ghost that drifts around the ego's path, with a trajectory line (belief geometry) ahead of it
// - about 30 boxes under perception, moving, appearing and disappearing
// - a drivable-area triangle chunk under belief, around the circle
//
// Ids use the Driver source byte 1, never the helper range. Everything is deterministic for a given seed.
//
// Owned and used by the simulation thread, like the scene it writes.

#include "avis/scene/scene.h"
#include "avis/scene/types.h"
#include "avis/simulation/time.h"

#include <Eigen/Dense>

#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

namespace avis::visualizer::simulation
{
    // The id of the n-th Driver entity: source byte 1 (08 §3)
    constexpr scene::object_id demo_entity_id(const std::uint64_t n) noexcept
    {
        return (std::uint64_t{ 1 } << 56) | n;
    }

    class demo_scene
    {
    public:
        // Ten ticks a second; tick k is at time k × tick_interval
        static constexpr std::chrono::milliseconds tick_interval{ 100 };

        static constexpr std::uint32_t box_count = 30;

        static constexpr scene::object_id driver_id(const std::uint64_t n) noexcept
        {
            return demo_entity_id(n);
        }

        static constexpr scene::object_id ego_vehicle_id = demo_entity_id(1);
        static constexpr scene::object_id shadow_vehicle_id = demo_entity_id(5);

        explicit demo_scene(std::uint32_t seed = 1);

        // Step 1 of the tick sequence (08 §9.6): writes this tick's components. Creates the entities on the first tick,
        // and again when they're gone (after *Reset*).
        void tick(scene::scene& scene, avis::simulation::step_index step, avis::simulation::simulation_duration time);

    private:
        struct box
        {
            scene::object_id id = 0;
            Eigen::Vector3f velocity = Eigen::Vector3f::Zero(); // m/s, world
            avis::simulation::simulation_duration expires{ 0 };
        };

        void create_entities(
            scene::scene& scene, avis::simulation::step_index step, avis::simulation::simulation_duration time);
        void move_vehicles(
            scene::scene& scene, avis::simulation::step_index step, avis::simulation::simulation_duration time);
        void update_boxes(
            scene::scene& scene, avis::simulation::step_index step, avis::simulation::simulation_duration time);
        void spawn_box(
            scene::scene& scene, avis::simulation::step_index step, avis::simulation::simulation_duration time);

        std::mt19937 random_;
        std::vector<box> boxes_;
        std::uint64_t next_box_ = 1000; // box ids count up from here, never reused
    };
} // namespace avis::visualizer::simulation

#endif
