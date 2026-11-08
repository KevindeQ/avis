#ifndef AVIS_VISUALIZER_UI_RENDERER_BRIDGE_H
#define AVIS_VISUALIZER_UI_RENDERER_BRIDGE_H

// The Qt side of rendering::frame_sink (docs/design/12-visualizer.md §4, "Renderer → GUI"): the frame loop calls it on
// a parallel worker, and each call is forwarded to the GUI thread with a queued call, where the bridge keeps the
// latest values and emits a signal. The Render Debug panel (12.15), picking (12.12) and screenshots (12.15) read it.
//
// Lives on the GUI thread; the frame_sink calls are the only ones made from other threads. A call queued after the
// bridge is gone is dropped.

#include "rendering/frame_loop.h"

#include <QObject>
#include <QString>

#include <vector>

namespace avis::visualizer::ui
{
    class renderer_bridge final : public QObject, public rendering::frame_sink
    {
        Q_OBJECT

    public:
        explicit renderer_bridge(QObject* parent = nullptr);

        // ---- frame_sink: the frame loop ----
        void debug_information_changed(rendering::frame_debug_information information) override;
        void picks_completed(std::vector<rendering::pick_result> results) override;
        void inspections_completed(std::vector<rendering::inspector_result> results) override;
        void screenshots_completed(std::vector<rendering::screenshot_result> results) override;
        void fatal_error(avis::error error) override;

        // ---- GUI thread ----

        const rendering::frame_debug_information& debug_information() const noexcept
        {
            return debug_information_;
        }

        // The results received since the last call, oldest first
        std::vector<rendering::pick_result> take_picks();
        std::vector<rendering::inspector_result> take_inspections();
        std::vector<rendering::screenshot_result> take_screenshots();

    Q_SIGNALS:
        void debug_information_updated();
        // Emitted once until the matching take_* call
        void picks_available();
        void inspections_available();
        void screenshots_available();
        // The device was lost (12 §7); `message` names the cause
        void fatal_error_occurred(const QString& message);

    private:
        rendering::frame_debug_information debug_information_;
        std::vector<rendering::pick_result> picks_;
        std::vector<rendering::inspector_result> inspections_;
        std::vector<rendering::screenshot_result> screenshots_;
    };
} // namespace avis::visualizer::ui

#endif
