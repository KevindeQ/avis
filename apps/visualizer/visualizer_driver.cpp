#include "visualizer_driver.h"

#include "avis/log.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/transient.h"
#include "avis/runtime.h"
#include "avis/threading/parallel_helpers.h"

#include <QCoreApplication>

#include <filesystem>
#include <format>
#include <system_error>
#include <utility>

namespace
{
    // The proxy model of 08 §5.1, relative to the data directory
    const std::filesystem::path vehicle_proxy_path = std::filesystem::path{ "data" } / "assets" / "vehicle_proxy.glb";

    // The first existing data file: under the working directory (a run from the source tree), then next to the
    // executable. Empty when neither exists.
    std::filesystem::path find_data_file(const std::filesystem::path& relative)
    {
        const std::filesystem::path candidates[] = {
            relative,
            std::filesystem::path{ QCoreApplication::applicationDirPath().toStdWString() } / relative,
        };
        for (const std::filesystem::path& candidate : candidates)
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error))
            {
                return candidate;
            }
        }
        return {};
    }
} // namespace

visualizer_driver::visualizer_driver(QObject* parent) : QObject(parent), assets_{ avis::runtime::io_context() }
{
    load_vehicle_proxy();

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

visualizer_driver::~visualizer_driver()
{
    if (vehicle_proxy_)
    {
        assets_.release_all(*vehicle_proxy_);
    }
}

void visualizer_driver::load_vehicle_proxy()
{
    const std::filesystem::path path = find_data_file(vehicle_proxy_path);
    if (path.empty())
    {
        avis::log::get("scene")->info(
            "no vehicle proxy model ({}): vehicles are drawn as boxes", vehicle_proxy_path.string());
        return;
    }

    // At startup, on the GUI thread, where waiting is allowed; the model is small
    avis::result<avis::scene::gltf_asset_set> loaded = avis::threading::sync_wait_result(assets_.load_gltf(path));
    if (!loaded)
    {
        avis::log::get("scene")->error(
            "the vehicle proxy model {} could not be loaded: {}", path.string(), std::format("{}", loaded.error()));
        return;
    }
    avis::log::get("scene")->info(
        "loaded the vehicle proxy model {}: {} meshes, {} materials, {} textures",
        path.string(),
        loaded->meshes.size(),
        loaded->materials.size(),
        loaded->textures.size());
    vehicle_proxy_ = std::move(*loaded);
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
