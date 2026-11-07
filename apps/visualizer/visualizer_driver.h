#ifndef VISUALIZER_VISUALIZER_DRIVER_H
#define VISUALIZER_VISUALIZER_DRIVER_H

#include "simulation/simulation_thread.h"
#include "ui/simulation_bridge.h"
#include "visualizer_configuration.h"

#include "avis/error.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/gltf.h"
#include "avis/scene/snapshot.h"

#include <QObject>
#include <QPointer>
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
    class shared_frame_rings;
} // namespace avis::visualizer::rendering

namespace avis::visualizer::ui
{
    struct qt_adapter;
    class viewport_item;
} // namespace avis::visualizer::ui

// The application controller (12 §2). main() creates the one instance on the GUI thread, before the QML engine, and
// QML reaches it as the VisualizerDriver singleton. It owns, in this order: the configuration, the asset cache, the
// snapshot buffer, the simulation thread (which owns the scene, 12.6), then (from 12.7) the frame loop. The graphics
// objects (rendering::render_system, the RHI's only holder), the shared frame rings and the completion notifier follow
// once Qt's adapter is known (12 §3.2).
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
    // `configuration_path` is where the configuration is written back (12.16). With `demo_scene` (--demo-scene), the
    // demo scene stands in for the Driver (12.6).
    visualizer_driver(
        avis::visualizer::visualizer_configuration configuration,
        std::filesystem::path configuration_path,
        bool demo_scene);
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

    // Starts graphics on Qt's adapter once the window's scene graph is initialised (12 §3.2 steps 2 and 3), and gives
    // the window's Viewport the shared frame rings then (step 4). Call once, on the GUI thread, right after the QML
    // engine created the window, before the event loop shows it.
    void watch_window(QQuickWindow& window);

    // Any thread: a command for the simulation thread (12 §4.1); dropped when the thread couldn't start
    void post(avis::visualizer::simulation::simulation_command command);

    // What the simulation thread sends the GUI: the scene tree's changes, the selected entity, statistics, the follow
    // target
    avis::visualizer::ui::simulation_bridge& simulation_updates() noexcept
    {
        return simulation_updates_;
    }

Q_SIGNALS:
    void startup_error_changed();

private:
    // Loads data/assets/vehicle_proxy.glb when it exists (08 §5.1); the representation task adds the file
    void load_vehicle_proxy();

    // On the GUI thread, with what read_qt_adapter found on the render thread: creates the render system on Qt's
    // adapter, the shared frame rings, then the completion notifier. A failure is the startup error (12 §7).
    void start_graphics(const std::optional<avis::visualizer::ui::qt_adapter>& adapter);
    void set_startup_error(const QString& message);

    // Runs the RHI's housekeeping when its completion event fires (12 §3.4, D-93)
    void poll_completions();

    // Starts the simulation thread on the runtime's thread pool; a failure is the startup error
    void start_simulation(bool demo_scene);

    // visualizer.yaml, as loaded (12.1), and where it's written back
    avis::visualizer::visualizer_configuration configuration_;
    std::filesystem::path configuration_path_;

    // Declared before the simulation thread, whose scene releases its assets on destruction. glTF loads read through
    // the runtime's IO context: main() creates the runtime before this controller, and destroys it after.
    avis::scene::asset_cache assets_;

    // One reference to each asset of the proxy model, when it was loaded
    std::optional<avis::scene::gltf_asset_set> vehicle_proxy_;

    // The scene's snapshots. Declared before the renderer, which reads one only while it renders.
    avis::scene::snapshot_buffer snapshots_;

    // Where the simulation thread's updates arrive on the GUI thread; outlives the thread
    avis::visualizer::ui::simulation_bridge simulation_updates_;

    // Owns the scene (12.6). Stopped first at shutdown (12 §3.4). Null when it couldn't start.
    std::unique_ptr<avis::visualizer::simulation::simulation_thread> simulation_;

    QString startup_error_;
    bool graphics_requested_ = false; // the window's first sceneGraphInitialized arrived

    // Null until Qt's adapter is known, and when graphics couldn't start (startup_error_ says why)
    std::unique_ptr<avis::visualizer::rendering::render_system> graphics_;

    // The rings that carry frames to the viewport (12 §3.3), with graphics_; the frame loop (12.7) writes them. The
    // window, and with it the viewport's renderer on Qt's render thread, is gone before they're destroyed: main()'s QML
    // engine is destroyed before this controller.
    std::unique_ptr<avis::visualizer::rendering::shared_frame_rings> frame_rings_;

    // The window's Viewport, found by watch_window; null when the QML has none
    QPointer<avis::visualizer::ui::viewport_item> viewport_;

    // In the GUI event loop, on the RHI's completion event; destroyed before graphics_
    std::unique_ptr<QWinEventNotifier> completion_notifier_;
};

#endif
