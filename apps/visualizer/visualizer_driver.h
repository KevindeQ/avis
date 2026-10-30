#ifndef VISUALIZER_VISUALIZER_DRIVER_H
#define VISUALIZER_VISUALIZER_DRIVER_H

#include "rendering/scene_renderer.h"
#include "simulator_clock.h"

#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/transient_resource_pool.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/scene/scene.h"

#include <QObject>
#include <qqmlintegration.h>

#include <chrono>
#include <memory>

using namespace std::literals::chrono_literals;

class visualizer_driver : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(VisualizerDriver)
    QML_SINGLETON

public:
    explicit visualizer_driver(QObject* parent = nullptr);

    Q_INVOKABLE void tick();

private:
    void on_update(std::chrono::milliseconds delta_time);
    void on_render();

    // Wall-clock time tracking
    std::chrono::steady_clock::time_point current_time_ = std::chrono::steady_clock::now();
    std::chrono::milliseconds frame_time_ = 16ms;
    std::chrono::milliseconds max_frame_time = 100ms;
    std::chrono::milliseconds accumulator_ = 0ms;

    avis::scene::scene current_scene_;

    // Simulation time tracking
    avis::virtual_clock_state clock_state_;

    // Null when the RHI couldn't be created; the error is logged. Destroyed last: the graphics objects below use it.
    std::unique_ptr<avis::rhi::render_hardware_interface> rhi_;

    // The render graph and what it executes with; null without an RHI or when the scheduler couldn't be created.
    // on_render only builds and compiles the graph: executing frames needs the frame chain and a completion driver
    // (12).
    std::unique_ptr<avis::graphics::frame_scheduler> frames_;
    std::unique_ptr<avis::graphics::transient_resource_pool> transient_pool_;
    std::unique_ptr<avis::graphics::render_graph> graph_;
    rendering::scene_renderer renderer_;
};

#endif
