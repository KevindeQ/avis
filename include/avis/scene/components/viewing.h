#ifndef AVIS_SCENE_COMPONENTS_VIEWING_H
#define AVIS_SCENE_COMPONENTS_VIEWING_H

// Viewing aids (docs/design/08-scene-management.md §8): labels, trails and update stamps. The scene library keeps the
// data; the Visualizer's scene representation turns labels and trails into meshes.

#include "avis/scene/types.h"
#include "avis/simulation/time.h"

#include <Eigen/Dense>

#include <deque>
#include <initializer_list>
#include <optional>
#include <string>

namespace avis::scene::components
{
    // ---- label (§8.1) ----

    struct label_style
    {
        color text_color{ 1.0f, 1.0f, 1.0f, 1.0f };
        color background{ 0.0f, 0.0f, 0.0f, 0.5f }; // a quad behind the text; alpha 0 = none
        float pixel_height = 14.0f; // constant on screen, scaled by the device pixel ratio
        float maximum_view_distance = 80.0f; // metres; farther labels aren't drawn
    };

    // A text above the entity, added by the representation from its configuration, never by the Driver
    struct label
    {
        std::string text_template; // "{name} · {property:perception.speed:.1f}" (scene::format_label)
        Eigen::Vector3f anchor_offset = Eigen::Vector3f::Zero(); // metres, in the entity's frame
        Eigen::Vector2f pixel_offset = Eigen::Vector2f::Zero(); // pixels, after projection
        label_style style;
    };

    // ---- trail (§8.2) ----

    struct trail_settings
    {
        float duration_seconds = 10.0f; // simulation time kept
        float minimum_spacing = 0.25f; // metres between samples
        color line_color{ 1.0f, 0.8f, 0.2f, 1.0f };
        float line_width = 2.0f; // pixels
    };

    struct trail_sample
    {
        simulation::simulation_duration time{ 0 };
        Eigen::Vector3f position = Eigen::Vector3f::Zero(); // world, relative to the scenario origin
        bool starts_segment = false; // after a discontinuity: no line from the previous sample
    };

    // `samples` is owned by scene: only scene::update_trails writes it (08 §9.2). A std::deque, as 08 §8.2 specifies;
    // MSVC's std::deque move constructor isn't noexcept, so trail is the one component that isn't nothrow-movable.
    struct trail
    {
        trail_settings settings;
        std::deque<trail_sample> samples; // oldest first
    };

    // ---- update_stamp (§8.4) ----

    struct update_time
    {
        simulation::step_index step = 0;
        simulation::simulation_duration time{ 0 };

        friend bool operator==(const update_time&, const update_time&) = default;
    };

    // When the Driver last created or changed the entity, per kind of change. Written by the Visualizer's scene_stream.
    struct update_stamp
    {
        update_time created;
        std::optional<update_time> pose; // transform_update, vehicle_state
        std::optional<update_time> properties; // entity_properties
        std::optional<update_time> geometry; // geometry_update, lidar_sweep

        // The newest of the four, by step. On a tie, the first in the order created, pose, properties, geometry.
        update_time latest() const noexcept
        {
            update_time newest = created;
            for (const std::optional<update_time>* change : { &pose, &properties, &geometry })
            {
                if (change->has_value() && (*change)->step > newest.step)
                {
                    newest = **change;
                }
            }
            return newest;
        }
    };
} // namespace avis::scene::components

#endif
