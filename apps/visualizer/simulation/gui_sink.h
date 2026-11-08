#ifndef AVIS_VISUALIZER_SIMULATION_GUI_SINK_H
#define AVIS_VISUALIZER_SIMULATION_GUI_SINK_H

// The simulation → GUI path (docs/design/12-visualizer.md §4, 08 §7). The simulation thread calls it with value types;
// the Qt side (ui/simulation_bridge) forwards each call to the GUI thread with a queued call, and tests record them.
// Every call comes from the simulation thread and must not block.

#include "representation/scene_representation.h"

#include "avis/scene/bounds.h"
#include "avis/scene/reflection.h"
#include "avis/scene/scene.h"
#include "avis/scene/statistics.h"
#include "avis/scene/types.h"

#include <optional>
#include <vector>

namespace avis::visualizer::simulation
{
    // The entity the follow camera chases (12 §5): the selected vehicle, otherwise the primary one
    struct follow_target
    {
        scene::object_id id = 0; // 0 = none
        std::optional<scene::aabb> bounds; // world bounds of what it draws (08 §8.3); none when it draws nothing

        friend bool operator==(const follow_target&, const follow_target&) = default;
    };

    class gui_sink
    {
    public:
        virtual ~gui_sink() = default;

        // The scene tree's changes since the last call, after a structural change. Never empty.
        virtual void tree_changed(std::vector<scene::tree_change> changes) = 0;

        // The selected entity, at up to 20 Hz while it changes, or nullopt once the selection is cleared
        virtual void selected_entity_changed(std::optional<scene::reflected_entity> entity) = 0;

        // At 2 Hz (08 §8.5), with the representation's reduction level (12 §9.1)
        virtual void statistics_changed(
            const scene::scene_statistics& statistics,
            const representation::representation_statistics& representation) = 0;

        // When the target changes, and when its extent changes
        virtual void follow_target_changed(const follow_target& target) = 0;

    protected:
        gui_sink() = default;
        gui_sink(const gui_sink&) = default;
        gui_sink& operator=(const gui_sink&) = default;
    };
} // namespace avis::visualizer::simulation

#endif
