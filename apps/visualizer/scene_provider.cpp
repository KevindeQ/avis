#include "scene_provider.h"

#include <QDebug>
#include <QQmlEngine>
#include <QRunnable>
#include <QThread>

namespace
{
    constexpr int scene_width = 800;
    constexpr int scene_height = 600;
} // namespace

class cleanup_job : public QRunnable
{
    Q_DISABLE_COPY(cleanup_job)
public:
    explicit cleanup_job(scene_provider_renderer* renderer) : renderer_{ renderer }
    {}

    void run() override
    {
        delete renderer_;
    }

private:
    scene_provider_renderer* renderer_;
};

scene_provider::scene_provider(QQuickItem* parent) : QQuickItem(parent)
{
    connect(this, &QQuickItem::windowChanged, this, &scene_provider::on_window_changed);
}

void scene_provider::declare_qml()
{
    qmlRegisterType<scene_provider>("Avis", 1, 0, "SceneProvider");
}

void scene_provider::releaseResources()
{
    window()->scheduleRenderJob(new cleanup_job(renderer_), QQuickWindow::BeforeSynchronizingStage);
    renderer_ = nullptr;
}

void scene_provider::on_window_changed(QQuickWindow* window)
{
    connection_initialized_ = connect(
        window,
        &QQuickWindow::sceneGraphInitialized,
        [this, window]
        {
            disconnect(connection_initialized_);

            // We are currently in a render thread
            renderer_ = new scene_provider_renderer();
            renderer_->init(window, { scene_width, scene_height });

            // Make sure we are connecting directly so the signals won't get queued
            connect(
                window,
                &QQuickWindow::beforeSynchronizing,
                renderer_,
                &scene_provider_renderer::synchronize,
                Qt::DirectConnection);
            connect(
                window,
                &QQuickWindow::beforeRendering,
                renderer_,
                &scene_provider_renderer::render,
                Qt::DirectConnection);
            connect(
                window,
                &QQuickWindow::sceneGraphInvalidated,
                renderer_,
                &scene_provider_renderer::cleanup,
                Qt::DirectConnection);
        });
}
