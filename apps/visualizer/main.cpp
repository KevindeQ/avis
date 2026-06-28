#include "scene_provider.h"
#include "simulator_driver.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>

int main(int argc, char* argv[])
{
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