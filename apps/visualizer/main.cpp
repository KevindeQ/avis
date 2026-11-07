#include "log_location.h"

#include "avis/runtime.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <system_error>

namespace
{
    // Hard-coded until the Visualizer loads visualizer.yaml (with runtime_configuration::parse)
    avis::runtime_configuration make_runtime_configuration(const std::filesystem::path& log_file)
    {
        avis::runtime_configuration configuration;
        configuration.application_threads = 1; // simulation; the renderer is a sender chain
        configuration.logging.file = log_file;
        return configuration;
    }
} // namespace

int main(int argc, char* argv[])
{
    // Logging, thread pool, IO and the parallel pool, for the whole of main
    // The log goes next to the executable; argv[0] is resolved before anything changes the working directory
    std::error_code path_error;
    const std::filesystem::path executable = std::filesystem::absolute(std::filesystem::path{ argv[0] }, path_error);
    visualizer_log::file_path() = executable.parent_path() / visualizer_log::file_name;

    std::optional<avis::runtime> runtime;
    try
    {
        runtime.emplace(make_runtime_configuration(visualizer_log::file_path()));
    }
    catch (const avis::runtime_error& error)
    {
        std::fputs(std::format("avis: failed to start the runtime: {}\n", error.error()).c_str(), stderr);
        return 1;
    }

    // Force usage of Direct3D12 for rendering
    QQuickWindow::setGraphicsApi(QSGRendererInterface::GraphicsApi::Direct3D12);

    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.load(QUrl{ "qrc:///avis/imports/visualizer/main.qml" });

    return app.exec();
}
