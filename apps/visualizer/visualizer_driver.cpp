#include "visualizer_driver.h"

#include "avis/log.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/transient.h"

#include <format>
#include <utility>

visualizer_driver::visualizer_driver(QObject* parent) : QObject(parent)
{
    avis::rhi::render_hardware_interface_configuration configuration;
    configuration.debug.debug_layer = true;

    auto created = avis::rhi::render_hardware_interface::create(configuration);
    if (!created)
    {
        avis::log::get("ui")->error("the RHI could not be created: {}", std::format("{}", created.error()));
        return;
    }
    rhi_ = std::move(*created);

    auto frames = avis::graphics::frame_scheduler::create(
        avis::rhi::synchronization{ *rhi_ }, avis::rhi::transient_objects{ *rhi_ }, {});
    if (!frames)
    {
        avis::log::get("ui")->error("the frame scheduler could not be created: {}", std::format("{}", frames.error()));
        return;
    }
    frames_ = std::move(*frames);
    transient_pool_ = std::make_unique<avis::graphics::transient_resource_pool>(
        avis::rhi::memory{ *rhi_ }, avis::rhi::resources{ *rhi_ }, frames_->frames_in_flight());
    graph_ = std::make_unique<avis::graphics::render_graph>(
        avis::rhi::resources{ *rhi_ },
        avis::rhi::command_lists{ *rhi_ },
        avis::rhi::queue<avis::rhi::queue_type::graphics>{ *rhi_ },
        avis::rhi::queue<avis::rhi::queue_type::compute>{ *rhi_ },
        avis::rhi::queue<avis::rhi::queue_type::copy>{ *rhi_ },
        *transient_pool_,
        *frames_);
}

void visualizer_driver::tick()
{
    // Update the simulation using a fixed timestep

    const auto new_time = std::chrono::steady_clock::now();
    auto delta_time = std::chrono::duration_cast<std::chrono::milliseconds>(new_time - current_time_);

    // Clamp the delta_time to don't do too many simulation steps when the time was frozen due to debugging
    if (delta_time > max_frame_time)
    {
        delta_time = max_frame_time;
    }

    accumulator_ += delta_time;
    while (accumulator_ >= frame_time_)
    {
        on_update(frame_time_);

        accumulator_ -= frame_time_;
    }

    on_render();

    current_time_ = new_time;
}

void visualizer_driver::on_update(const std::chrono::milliseconds delta_time)
{
    clock_state_.advance(delta_time);
}

void visualizer_driver::on_render()
{
    if (graph_ == nullptr)
    {
        return;
    }
    // Build and compile only: executing needs the frame chain and a completion driver (12). The viewport's size
    // comes with them too.
    renderer_.render(*graph_, current_scene_, { 1280, 720 });
}
