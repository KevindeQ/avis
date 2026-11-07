#ifndef VISUALIZER_VISUALIZER_DRIVER_H
#define VISUALIZER_VISUALIZER_DRIVER_H

#include "visualizer_configuration.h"

#include "avis/error.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/gltf.h"
#include "avis/scene/scene.h"
#include "avis/scene/snapshot.h"

#include <QObject>
#include <qqmlintegration.h>
#include <QString>

#include <filesystem>
#include <memory>
#include <optional>

class QJSEngine;
class QQmlEngine;
class QQuickWindow;
class QWinEventNotifier;

namespace avis::visualizer::rendering
{
    class render_system;
}

namespace avis::visualizer::ui
{
    struct qt_adapter;
}

// The application controller (12 §2). main() creates the one instance on the GUI thread, before the QML engine, and
// QML reaches it as the VisualizerDriver singleton. It owns, in this order: the configuration, the asset cache, the
// scene, the snapshot buffer, then (from 12.6 and 12.7) the simulation thread and the frame loop. The graphics objects
// (rendering::render_system, the RHI's only holder) and the completion notifier follow once Qt's adapter is known
// (12 §3.2).
class visualizer_driver : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(VisualizerDriver)
    QML_SINGLETON

    // Why the Visualizer can't run, empty while it can (12 §7, 12a §5): main.qml shows it in a modal dialog, then
    // exits. Set at most once, when graphics fail to start.
    Q_PROPERTY(QString startupError READ startup_error NOTIFY startup_error_changed)
    // The log file the dialog names (log_location.h)
    Q_PROPERTY(QString logFile READ log_file CONSTANT)

public:
    // `configuration_path` is where the configuration is written back (12.16)
    visualizer_driver(
        avis::visualizer::visualizer_configuration configuration, std::filesystem::path configuration_path);
    ~visualizer_driver() override; // the shutdown order of 12 §3.4

    visualizer_driver(const visualizer_driver&) = delete;
    visualizer_driver& operator=(const visualizer_driver&) = delete;

    // The QML singleton: the instance main() created. The engine doesn't own it.
    static visualizer_driver* create(QQmlEngine* engine, QJSEngine* script_engine);

    QString startup_error() const
    {
        return startup_error_;
    }

    QString log_file() const;

    // Starts graphics on Qt's adapter once the window's scene graph is initialised (12 §3.2 steps 2 and 3). Call once,
    // on the GUI thread, right after the QML engine created the window, before the event loop shows it.
    void watch_window(QQuickWindow& window);

Q_SIGNALS:
    void startup_error_changed();

private:
    // Loads data/assets/vehicle_proxy.glb when it exists (08 §5.1); the representation task adds the file
    void load_vehicle_proxy();

    // On the GUI thread, with what read_qt_adapter found on the render thread: creates the render system on Qt's
    // adapter, then the completion notifier. A failure is the startup error (12 §7).
    void start_graphics(const std::optional<avis::visualizer::ui::qt_adapter>& adapter);
    void set_startup_error(const QString& message);

    // Runs the RHI's housekeeping when its completion event fires (12 §3.4, D-93)
    void poll_completions();

    // The tick sequence of 08 §9.6 into snapshots_: update the transforms, make the snapshot, publish, end the tick
    void publish_snapshot();

    // visualizer.yaml, as loaded (12.1), and where it's written back
    avis::visualizer::visualizer_configuration configuration_;
    std::filesystem::path configuration_path_;

    // Declared before the scene, which releases its assets on destruction. glTF loads read through the runtime's IO
    // context: main() creates the runtime before this controller, and destroys it after.
    avis::scene::asset_cache assets_;
    avis::scene::scene current_scene_{ assets_ };

    // One reference to each asset of the proxy model, when it was loaded
    std::optional<avis::scene::gltf_asset_set> vehicle_proxy_;

    // The scene's snapshots. Declared before the renderer, which reads one only while it renders.
    avis::scene::snapshot_buffer snapshots_;

    QString startup_error_;
    bool graphics_requested_ = false; // the window's first sceneGraphInitialized arrived

    // Null until Qt's adapter is known, and when graphics couldn't start (startup_error_ says why)
    std::unique_ptr<avis::visualizer::rendering::render_system> graphics_;

    // In the GUI event loop, on the RHI's completion event; destroyed before graphics_
    std::unique_ptr<QWinEventNotifier> completion_notifier_;
};

#endif
