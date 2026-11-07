#ifndef AVIS_VISUALIZER_UI_SIMULATION_BRIDGE_H
#define AVIS_VISUALIZER_UI_SIMULATION_BRIDGE_H

// The Qt side of simulation::gui_sink (docs/design/12-visualizer.md §4): the simulation thread calls it, and each call
// is forwarded to the GUI thread with a queued call, where the bridge keeps the latest values and emits a signal. The
// UI models (tasks 12.13, 12.14) and the camera (12.12) read from it.
//
// Lives on the GUI thread; the gui_sink calls are the only ones made from the simulation thread. A call queued after
// the bridge is gone is dropped.

#include "simulation/gui_sink.h"

#include "avis/scene/reflection.h"
#include "avis/scene/scene.h"
#include "avis/scene/statistics.h"

#include <QObject>

#include <optional>
#include <vector>

namespace avis::visualizer::ui
{
    class simulation_bridge final : public QObject, public simulation::gui_sink
    {
        Q_OBJECT

    public:
        explicit simulation_bridge(QObject* parent = nullptr);

        // ---- gui_sink: the simulation thread ----
        void tree_changed(std::vector<scene::tree_change> changes) override;
        void selected_entity_changed(std::optional<scene::reflected_entity> entity) override;
        void statistics_changed(const scene::scene_statistics& statistics) override;
        void follow_target_changed(const simulation::follow_target& target) override;

        // ---- GUI thread ----

        // The tree changes received since the last call, oldest first
        std::vector<scene::tree_change> take_tree_changes();

        const std::optional<scene::reflected_entity>& selected_entity() const noexcept
        {
            return selected_;
        }

        const scene::scene_statistics& statistics() const noexcept
        {
            return statistics_;
        }

        const simulation::follow_target& follow_target() const noexcept
        {
            return follow_;
        }

    Q_SIGNALS:
        void tree_changes_available();
        void selected_entity_updated();
        void statistics_updated();
        void follow_target_updated();

    private:
        std::vector<scene::tree_change> tree_changes_;
        std::optional<scene::reflected_entity> selected_;
        scene::scene_statistics statistics_;
        simulation::follow_target follow_;
    };
} // namespace avis::visualizer::ui

#endif
