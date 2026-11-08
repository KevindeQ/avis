#ifndef AVIS_VISUALIZER_REPRESENTATION_PRESENTATION_STATE_H
#define AVIS_VISUALIZER_REPRESENTATION_PRESENTATION_STATE_H

// What the presentation commands (docs/design/12-visualizer.md §4.1) have set: the state the scene representation
// (12 §9) draws the scene with. The simulation thread owns it and changes it only between ticks; the representation
// reads it at step 4 of the tick sequence (08 §9.6).

#include "representation/representation_settings.h"

#include "avis/scene/snapshot.h"
#include "avis/scene/types.h"

#include <unordered_map>

namespace avis::visualizer::representation
{
    enum class helper_kind : std::uint8_t
    {
        grid,
        axes_gizmo
    };

    // How lidar points are coloured (the scenario's visualization.lidar.color_mode, 04 §2.2). A sweep without the
    // channel a mode needs is drawn uniform.
    enum class lidar_color_mode : std::uint8_t
    {
        intensity,
        height, // world z
        ring,
        uniform
    };

    // The scenario's visualization.lidar section (04 §2.2): how sweeps look. A presentation setting: it changes in any
    // clock state (set_lidar_view).
    struct lidar_view
    {
        lidar_color_mode color_mode = lidar_color_mode::intensity;
        float point_size = 0.05f; // metres (at least 1 px on screen)

        friend bool operator==(const lidar_view&, const lidar_view&) = default;
    };

    struct presentation_state
    {
        scene::object_id selected = 0; // 0 = nothing selected

        // An entity is visible when its category is and it isn't hidden itself (an entry set to false)
        per_category<bool> category_visible = make_all(true);
        std::unordered_map<scene::object_id, bool> entity_visible;

        bool grid = true;
        bool axes_gizmo = true;

        representation_settings representation;

        // Labels and trails per category (the View menu), and trails per entity (the scene tree's context menu), which
        // win over their category
        per_category<bool> labels = make_all(false);
        per_category<bool> trails = make_all(false);
        std::unordered_map<scene::object_id, bool> entity_trails;

        bool stale = false; // staleness (08 §8.4)

        scene::environment_settings environment; // the scenario's `visualization` section (D-42)
        lidar_view lidar; // the scenario's visualization.lidar

        // The state at startup: labels where they're enabled and have a template, trails where they have a duration,
        // staleness as configured
        static presentation_state
            from_settings(const representation_settings& settings, const scene::environment_settings& environment = {});

        // An entity's visibility from these toggles, without its ancestors
        bool visible(scene::object_id id, scene::entity_category category) const noexcept;

        // Whether the entity has a trail: its own entry, otherwise its category
        bool trail(scene::object_id id, scene::entity_category category) const noexcept;

    private:
        static per_category<bool> make_all(bool value) noexcept;
    };
} // namespace avis::visualizer::representation

#endif
