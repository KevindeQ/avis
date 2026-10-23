#include "scene_provider.h"
#include "simulator_driver.h"

#include "avis/runtime.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <cstdio>
#include <format>
#include <optional>

namespace
{
    // Hard-coded until the Visualizer loads visualizer.yaml (with runtime_configuration::parse)
    avis::runtime_configuration make_runtime_configuration()
    {
        avis::runtime_configuration configuration;
        configuration.application_threads = 1; // simulation; the renderer is a sender chain
        return configuration;
    }
} // namespace

int main(int argc, char* argv[])
{
    // Logging, thread pool, IO and the parallel pool, for the whole of main
    std::optional<avis::runtime> runtime;
    try
    {
        runtime.emplace(make_runtime_configuration());
    }
    catch (const avis::runtime_error& error)
    {
        std::fputs(std::format("avis: failed to start the runtime: {}\n", error.error()).c_str(), stderr);
        return 1;
    }

    // Declare / Register all used custom QML elements
    scene_provider::declare_qml();
    simulator_driver::declare_qml();

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

    const QUrl url(u"qrc:/visualizer_qt/main.qml"_qs);
    engine.load(url);

    return app.exec();
}