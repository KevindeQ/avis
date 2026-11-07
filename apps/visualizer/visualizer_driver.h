#ifndef VISUALIZER_VISUALIZER_DRIVER_H
#define VISUALIZER_VISUALIZER_DRIVER_H

#include "visualizer_configuration.h"

#include "avis/error.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/pipeline_cache.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_library.h"
#include "avis/graphics/transient_resource_pool.h"
#include "avis/graphics/upload_queue.h"
#include "avis/rhi/render_hardware_interface.h"
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

namespace avis::visualizer::rendering
{
    class scene_renderer;
}

// The application controller (12 §2). main() creates the one instance on the GUI thread, before the QML engine, and
// QML reaches it as the VisualizerDriver singleton. It owns, in this order: the configuration, the asset cache, the
// scene, the snapshot buffer, then (from 12.6 and 12.7) the simulation thread and the frame loop, and the graphics
// objects until the frame loop takes them over.
class visualizer_driver : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(VisualizerDriver)
    QML_SINGLETON

    // Why the Visualizer can't run, empty when it started (12 §7, 12a §5): main.qml shows it in a modal dialog, then
    // exits. Set by the constructor and never changed afterwards.
    Q_PROPERTY(QString startupError READ startup_error CONSTANT)
    // The log file the dialog names (log_location.h)
    Q_PROPERTY(QString logFile READ log_file CONSTANT)

public:
    // `configuration_path` is where the configuration is written back (12.16)
    visualizer_driver(
        avis::visualizer::visualizer_configuration configuration, std::filesystem::path configuration_path);
    ~visualizer_driver() override; // waits for the GPU, then releases the renderer before what it uses

    visualizer_driver(const visualizer_driver&) = delete;
    visualizer_driver& operator=(const visualizer_driver&) = delete;

    // The QML singleton: the instance main() created. The engine doesn't own it.
    static visualizer_driver* create(QQmlEngine* engine, QJSEngine* script_engine);

    QString startup_error() const
    {
        return startup_error_;
    }

    QString log_file() const;

private:
    // Loads data/assets/vehicle_proxy.glb when it exists (08 §5.1); the representation task adds the file
    void load_vehicle_proxy();

    // The RHI, the frame scheduler and the graph, then the renderer's services, its shaders and the renderer itself
    // (12 §3.2 step 3, 12a §5). The first failure is the startup error.
    avis::result<void> create_graphics();
    avis::result<void> create_renderer();

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

    // Null when graphics couldn't start (startup_error_ says why). Destroyed last: the graphics objects below use it.
    std::unique_ptr<avis::rhi::render_hardware_interface> rhi_;

    // The render graph and what it executes with. Executing frames needs the frame chain and a completion driver (12).
    std::unique_ptr<avis::graphics::frame_scheduler> frames_;
    std::unique_ptr<avis::graphics::transient_resource_pool> transient_pool_;
    std::unique_ptr<avis::graphics::render_graph> graph_;

    // The renderer's services and shaders; each outlives the renderer, declared after them
    std::unique_ptr<avis::graphics::upload_queue> uploads_;
    std::unique_ptr<avis::graphics::root_signature_registry> root_signatures_;
    std::unique_ptr<avis::graphics::pipeline_cache> pipeline_cache_;
    std::unique_ptr<avis::graphics::shader_library> shaders_;
    std::unique_ptr<avis::visualizer::rendering::scene_renderer> renderer_;
};

#endif
