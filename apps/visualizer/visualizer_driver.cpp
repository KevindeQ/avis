#include "visualizer_driver.h"

#include "log_location.h"
#include "rendering/renderer_shaders.h"
#include "rendering/scene_renderer.h"

#include "avis/log.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/transient.h"
#include "avis/runtime.h"
#include "avis/threading/parallel_helpers.h"

#include <QCoreApplication>
#include <QJSEngine>
#include <QQmlEngine>

#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <utility>

namespace
{
    namespace rendering = avis::visualizer::rendering;

    // The proxy model of 08 §5.1, relative to the data directory
    const std::filesystem::path vehicle_proxy_path = std::filesystem::path{ "data" } / "assets" / "vehicle_proxy.glb";

    // The one controller, set while it's alive (main() creates it)
    visualizer_driver* instance = nullptr;

    std::filesystem::path executable_directory()
    {
        return std::filesystem::path{ QCoreApplication::applicationDirPath().toStdWString() };
    }

    // The first existing data file: under the working directory (a run from the source tree), then next to the
    // executable. Empty when neither exists.
    std::filesystem::path find_data_file(const std::filesystem::path& relative)
    {
        const std::filesystem::path candidates[] = {
            relative,
            executable_directory() / relative,
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

    // Prefixes the step that failed to the error's message, keeping its domain and code
    avis::error startup_failure(avis::error error, const std::string_view step)
    {
        error.message = std::format("{}: {}", step, error.message);
        return error;
    }
} // namespace

visualizer_driver::visualizer_driver(
    avis::visualizer::visualizer_configuration configuration, std::filesystem::path configuration_path) :
    configuration_{ std::move(configuration) },
    configuration_path_{ std::move(configuration_path) },
    assets_{ avis::runtime::io_context() }
{
    Q_ASSERT(instance == nullptr);
    instance = this;

    load_vehicle_proxy();
    publish_snapshot();

    avis::result<void> started = create_graphics().and_then([this] { return create_renderer(); });
    if (!started)
    {
        // A startup error (12 §7, 12a §5): main.qml shows it and exits
        avis::log::get("ui")->error("the Visualizer could not start: {}", std::format("{}", started.error()));
        startup_error_ = QString::fromStdString(started.error().message);
    }
}

visualizer_driver::~visualizer_driver()
{
    // The renderer's upload flushes may still run on the copy queue; the members after rhi_ go in reverse order
    if (rhi_ != nullptr)
    {
        rhi_->wait_idle();
    }
    renderer_.reset();
    if (vehicle_proxy_)
    {
        assets_.release_all(*vehicle_proxy_);
    }
    instance = nullptr;
}

visualizer_driver* visualizer_driver::create(QQmlEngine* engine, QJSEngine* /*script_engine*/)
{
    Q_ASSERT(instance != nullptr);
    Q_ASSERT(engine->thread() == instance->thread());
    QJSEngine::setObjectOwnership(instance, QJSEngine::CppOwnership);
    return instance;
}

QString visualizer_driver::log_file() const
{
    return QString::fromStdWString(visualizer_log::file_path().wstring());
}

avis::result<void> visualizer_driver::create_graphics()
{
    avis::rhi::render_hardware_interface_configuration configuration;
    configuration.debug.debug_layer = true;

    auto created = avis::rhi::render_hardware_interface::create(configuration);
    if (!created)
    {
        return std::unexpected(startup_failure(std::move(created.error()), "the graphics device could not be created"));
    }
    rhi_ = std::move(*created);

    auto frames = avis::graphics::frame_scheduler::create(
        avis::rhi::synchronization{ *rhi_ }, avis::rhi::transient_objects{ *rhi_ }, {});
    if (!frames)
    {
        return std::unexpected(startup_failure(std::move(frames.error()), "the frame scheduler could not be created"));
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
    return {};
}

avis::result<void> visualizer_driver::create_renderer()
{
    auto uploads = avis::graphics::upload_queue::create(
        avis::rhi::resources{ *rhi_ },
        avis::rhi::command_lists{ *rhi_ },
        avis::rhi::queue<avis::rhi::queue_type::copy>{ *rhi_ },
        avis::rhi::synchronization{ *rhi_ },
        {});
    if (!uploads)
    {
        return std::unexpected(startup_failure(std::move(uploads.error()), "the upload queue could not be created"));
    }
    uploads_ = std::move(*uploads);
    root_signatures_ = std::make_unique<avis::graphics::root_signature_registry>(avis::rhi::pipelines{ *rhi_ });
    pipeline_cache_ =
        std::make_unique<avis::graphics::pipeline_cache>(avis::rhi::pipelines{ *rhi_ }, *root_signatures_);

    // At startup, on the GUI thread, where waiting is allowed (12a §5)
    const std::filesystem::path shader_directory =
        rendering::resolve_shader_directory(configuration_.render, executable_directory());
    auto library = avis::threading::sync_wait_result(rendering::load_renderer_shaders(shader_directory));
    if (!library)
    {
        return std::unexpected(
            startup_failure(std::move(library.error()), "the renderer's shaders could not be loaded"));
    }
    shaders_ = std::make_unique<avis::graphics::shader_library>(std::move(*library));
    avis::log::get("render")->info("loaded the renderer's shaders from {}", shader_directory.string());

    auto renderer = rendering::scene_renderer::create(
        rendering::renderer_dependencies{
            .resources = avis::rhi::resources{ *rhi_ },
            .pipelines = avis::rhi::pipelines{ *rhi_ },
            .synchronization = avis::rhi::synchronization{ *rhi_ },
            .graph = *graph_,
            .pool = *transient_pool_,
            .frames = *frames_,
            .profiler = nullptr, // GPU timings come with 12's frame chain
            .pipeline_cache = *pipeline_cache_,
            .root_signatures = *root_signatures_,
            .uploads = *uploads_,
            .memory_budget = [this] { return rhi_->local_memory_budget(); },
        },
        configuration_.render,
        *shaders_);
    if (!renderer)
    {
        return std::unexpected(startup_failure(std::move(renderer.error()), "the renderer could not be created"));
    }
    renderer_ = std::move(*renderer);
    return {};
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

void visualizer_driver::publish_snapshot()
{
    current_scene_.update_world_transforms();
    avis::scene::snapshot& target = snapshots_.begin_write();
    current_scene_.make_snapshot(target);
    snapshots_.publish();
    current_scene_.end_tick();
}
