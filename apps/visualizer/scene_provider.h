#ifndef VISUALIZER_SCENE_PROVIDER_H
#define VISUALIZER_SCENE_PROVIDER_H

#include "scene_provider_renderer.h"

#include <QQuickItem>
#include <QQuickWindow>

class scene_provider : public QQuickItem
{
    Q_OBJECT
public:
    explicit scene_provider(QQuickItem* parent = nullptr);

    static void declare_qml();

protected:
    virtual void releaseResources() override;

private slots:
    void on_window_changed(QQuickWindow* window);

private:
    scene_provider_renderer* renderer_ = nullptr;

    QMetaObject::Connection connection_initialized_;
};

#endif
