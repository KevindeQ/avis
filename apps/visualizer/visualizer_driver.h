#ifndef VISUALIZER_VISUALIZER_DRIVER_H
#define VISUALIZER_VISUALIZER_DRIVER_H

#include "rendering/render_settings.h"
#include "simulator_clock.h"

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

#include <chrono>
#include <memory>
#include <optional>

namespace avis::visualizer::rendering
{
    class scene_renderer;
}

using namespace std::literals::chrono_literals;

class visualizer_driver : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(VisualizerDriver)
    QML_SINGLETON

    // Why the Visualizer can't run, empty when it started (12 §7, 12a §5): main.qml shows it in a modal dialog, then
    // exits. Set while the singleton is created and never changed afterwards.
    Q_PROPERTY(QString startupError READ startup_error CONSTANT)
    // The log file the dialog names (log_location.h)
    Q_PROPERTY(QString logFile READ log_file CONSTANT)

public:
    explicit visualizer_driver(QObject* parent = nullptr);
    ~visualizer_driver() override; // waits for the GPU, then releases the renderer before what it uses

    Q_INVOKABLE void tick();

    QString startup_error() const
    {
        return startup_error_;
    }

    QString log_file() const;

private:
    void on_update(std::chrono::milliseconds delta_time);
    void on_render();

    // Loads data/assets/vehicle_proxy.glb when it exists (08 §5.1); the representation task adds the file
    void load_vehicle_proxy();

    // The RHI, the frame scheduler and the graph, then the renderer's services, its shaders and the renderer itself
    // (12 §3.2 step 3, 12a §5). The first failure is the startup error.
    avis::result<void> create_graphics();
    avis::result<void> create_renderer();

    // The tick sequence of 08 §9.6 into snapshots_: update the transforms, make the snapshot, publish, end the tick
    void publish_snapshot();

    // Wall-clock time tracking
    std::chrono::steady_clock::time_point current_time_ = std::chrono::steady_clock::now();
    std::chrono::milliseconds frame_time_ = 16ms;
    std::chrono::milliseconds max_frame_time = 100ms;
    std::chrono::milliseconds accumulator_ = 0ms;

    // Declared before the scene, which releases its assets on destruction. glTF loads read through the runtime's IO
    // context: main() creates the runtime before the QML engine that creates this singleton, and destroys it after.
    avis::scene::asset_cache assets_;
    avis::scene::scene current_scene_{ assets_ };

    // One reference to each asset of the proxy model, when it was loaded
    std::optional<avis::scene::gltf_asset_set> vehicle_proxy_;

    // Simulation time tracking
    avis::virtual_clock_state clock_state_;

    // The scene's snapshots, read by on_render. Declared before the renderer, which reads one only while it renders.
    avis::scene::snapshot_buffer snapshots_;

    QString startup_error_;

    // The defaults of 11 §14 until 12 reads visualizer.yaml
    avis::visualizer::rendering::render_settings render_settings_;

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
    bool build_failing_ = false; // the last frame couldn't be built (logged once until one builds again)
};

#endif
