#include "visualizer_driver.h"

#include "log_location.h"
#include "rendering/render_system.h"
#include "rendering/renderer_shaders.h"
#include "ui/viewport_renderer.h"

#include "avis/log.h"
#include "avis/runtime.h"
#include "avis/threading/parallel_helpers.h"

#include <QCoreApplication>
#include <QJSEngine>
#include <QMetaObject>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QWinEventNotifier>

#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
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

    std::string luid_text(const avis::rhi::adapter_luid luid)
    {
        return std::format("{:08x}:{:08x}", static_cast<std::uint32_t>(luid.high), luid.low);
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
}

visualizer_driver::~visualizer_driver()
{
    // The shutdown order of 12 §3.4. The simulation thread (12.6) stops first and the frame loop's scope (12.7)
    // drains next; then the notifier goes, and the render system destroys the graphics objects in reverse and the
    // RHI last, waiting for the GPU.
    completion_notifier_.reset();
    graphics_.reset();
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

void visualizer_driver::watch_window(QQuickWindow& window)
{
    // Emitted on the render thread, where Qt's QRhi may be read; the result goes to the GUI thread. A later
    // re-initialisation (after sceneGraphInvalidated) is ignored by start_graphics.
    QObject::connect(
        &window,
        &QQuickWindow::sceneGraphInitialized,
        this,
        [this, &window]()
        {
            std::optional<avis::visualizer::ui::qt_adapter> adapter;
            if (QRhi* const rhi = window.rhi(); rhi != nullptr)
            {
                adapter = avis::visualizer::ui::read_qt_adapter(*rhi);
            }
            QMetaObject::invokeMethod(this, [this, adapter]() { start_graphics(adapter); }, Qt::QueuedConnection);
        },
        Qt::DirectConnection);
}

void visualizer_driver::start_graphics(const std::optional<avis::visualizer::ui::qt_adapter>& adapter)
{
    const auto log = avis::log::get("ui");
    if (graphics_requested_)
    {
        if (adapter && graphics_ != nullptr && adapter->luid != graphics_->capabilities().luid)
        {
            log->warn(
                "Qt's scene graph moved to {} (LUID {}); the renderer stays on {}",
                adapter->name.toStdString(),
                luid_text(adapter->luid),
                graphics_->capabilities().adapter_name);
        }
        return;
    }
    graphics_requested_ = true;

    if (!adapter)
    {
        set_startup_error("Qt Quick isn't rendering with Direct3D 12, so the renderer can't share its frames with it");
        return;
    }
    log->info("Qt Quick renders on {} (LUID {})", adapter->name.toStdString(), luid_text(adapter->luid));

    rendering::render_system_configuration configuration{
        .adapter = adapter->luid,
        .settings = configuration_.render,
        .shader_directory = rendering::resolve_shader_directory(configuration_.render, executable_directory()),
    };
    auto created = rendering::render_system::create(configuration);
    if (!created)
    {
        log->error("the Visualizer could not start: {}", std::format("{}", created.error()));
        set_startup_error(QString::fromStdString(created.error().message));
        return;
    }
    graphics_ = std::move(*created);

    // GPU waits complete from the GUI event loop (12 §3.4, D-93): the frame loop's begin_frame, the ring's
    // when_reached senders, uploads and readbacks. Continuations doing real work move to the parallel scheduler.
    completion_notifier_ = std::make_unique<QWinEventNotifier>(graphics_->completion_driver().completion_event());
    QObject::connect(completion_notifier_.get(), &QWinEventNotifier::activated, this, [this]() { poll_completions(); });
    completion_notifier_->setEnabled(true);
}

void visualizer_driver::set_startup_error(const QString& message)
{
    if (!startup_error_.isEmpty())
    {
        return;
    }
    startup_error_ = message;
    Q_EMIT startup_error_changed();
}

void visualizer_driver::poll_completions()
{
    if (graphics_ != nullptr)
    {
        graphics_->completion_driver().poll_completions();
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

void visualizer_driver::publish_snapshot()
{
    current_scene_.update_world_transforms();
    avis::scene::snapshot& target = snapshots_.begin_write();
    current_scene_.make_snapshot(target);
    snapshots_.publish();
    current_scene_.end_tick();
}
