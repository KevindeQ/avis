#include "scene_provider_renderer.h"

#include <QDebug>

scene_provider_renderer::scene_provider_renderer(QObject* parent) : QObject(parent) {}

scene_provider_renderer::~scene_provider_renderer()
{
    cleanup();
}

void scene_provider_renderer::init(QQuickWindow* window, const QSize& resolution)
{
    window_ = window;

    setup_rendering();
}

void scene_provider_renderer::setup_rendering() {}

void scene_provider_renderer::synchronize() {}

void scene_provider_renderer::render()
{
    /*window_->beginExternalCommands();

    window_->endExternalCommands();*/
}

void scene_provider_renderer::cleanup() {}
