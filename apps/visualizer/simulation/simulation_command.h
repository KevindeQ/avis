#ifndef AVIS_VISUALIZER_SIMULATION_SIMULATION_COMMAND_H
#define AVIS_VISUALIZER_SIMULATION_SIMULATION_COMMAND_H

// What the GUI sends the simulation thread (docs/design/12-visualizer.md §4, §4.1), through command_queue. Value
// types, so a command can be built on any thread.
//
// - Simulation commands drive the clock (03 §3). They're placeholders until the tick loop (task 12.19): the simulation
//   thread logs and ignores them.
// - Presentation commands change only what's shown. They work in every clock state, and the simulation thread
//   republishes the snapshot after them (03 §4.4).

#include "representation/presentation_state.h"
#include "representation/representation_settings.h"

#include "avis/scene/snapshot.h"
#include "avis/scene/types.h"

#include <variant>

namespace avis::visualizer::simulation
{
    // ---- simulation commands (03 §3) ----

    struct start_simulation
    {};

    struct pause_simulation
    {};

    struct resume_simulation
    {};

    struct single_step
    {};

    struct stop_simulation
    {};

    struct set_speed
    {
        double scale = 1.0; // 0.1 to 10; ignored with maximum
        bool maximum = false; // as fast as the Driver allows
    };

    // ---- presentation commands (12 §4.1) ----

    struct select_entity
    {
        scene::object_id id = 0; // a helper selects its owner (`represents`)
    };

    struct clear_selection
    {};

    struct set_entity_visibility
    {
        scene::object_id id = 0;
        bool visible = true;
    };

    struct set_category_visibility
    {
        scene::entity_category category = scene::entity_category::object;
        bool visible = true;
    };

    struct set_helper_visibility
    {
        representation::helper_kind helper = representation::helper_kind::grid;
        bool visible = true;
    };

    struct set_representation_settings
    {
        representation::representation_settings settings;
    };

    struct set_category_labels
    {
        scene::entity_category category = scene::entity_category::object;
        bool enabled = true;
    };

    struct set_category_trails
    {
        scene::entity_category category = scene::entity_category::vehicle;
        bool enabled = true;
    };

    struct set_entity_trail
    {
        scene::object_id id = 0;
        bool enabled = true;
    };

    struct set_staleness
    {
        bool enabled = false;
    };

    // The scenario's `visualization` section (sun, sky)
    struct set_visualization
    {
        scene::environment_settings environment;
    };

    // Destroys every Driver entity, so the scene keeps only the Visualizer's helpers
    struct reset_scene
    {};

    // The scenario's visualization.lidar (colour mode, point size). Unlike the sun and the sky, it changes in every
    // clock state: a new mode recolours the kept sweeps (new mesh versions).
    struct set_lidar_view
    {
        representation::lidar_view view;
    };

    using simulation_command = std::variant<
        start_simulation,
        pause_simulation,
        resume_simulation,
        single_step,
        stop_simulation,
        set_speed,
        select_entity,
        clear_selection,
        set_entity_visibility,
        set_category_visibility,
        set_helper_visibility,
        set_representation_settings,
        set_category_labels,
        set_category_trails,
        set_entity_trail,
        set_staleness,
        set_visualization,
        reset_scene,
        set_lidar_view>;

    // True for the commands that drive the clock
    constexpr bool is_simulation_command(const simulation_command& command) noexcept
    {
        return command.index() <= 5;
    }
} // namespace avis::visualizer::simulation

#endif
