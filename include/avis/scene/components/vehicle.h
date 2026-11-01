#ifndef AVIS_SCENE_COMPONENTS_VEHICLE_H
#define AVIS_SCENE_COMPONENTS_VEHICLE_H

// A vehicle and its last vehicle_state message (docs/design/08-scene-management.md §3, 02 §5.8.3). The pose itself is
// the entity's node.

#include "avis/simulation/time.h"

#include <Eigen/Dense>

namespace avis::scene::components
{
    struct vehicle_state
    {
        simulation::simulation_duration time{ 0 }; // the time the state refers to
        Eigen::Vector3f linear_velocity = Eigen::Vector3f::Zero(); // m/s, vehicle frame
        Eigen::Vector3f angular_velocity = Eigen::Vector3f::Zero(); // rad/s, vehicle frame
        Eigen::Vector3f acceleration = Eigen::Vector3f::Zero(); // m/s², vehicle frame
        float speed = 0.0f; // m/s, signed
        float steering_angle = 0.0f; // rad, road-wheel angle
    };

    struct vehicle
    {
        bool primary = false; // the vehicle of the vehicle model, the follow camera's default target
        vehicle_state state;
    };
} // namespace avis::scene::components

#endif
