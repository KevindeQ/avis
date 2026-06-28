#ifndef VISUALIZER_SCENE_PROVIDER_RENDERER_H
#define VISUALIZER_SCENE_PROVIDER_RENDERER_H

#include <QObject>
#include <QQuickWindow>

class scene_provider_renderer : public QObject
{
    Q_OBJECT
public:
    explicit scene_provider_renderer(QObject* parent = nullptr);
    ~scene_provider_renderer();

public slots:
    void init(QQuickWindow* window, const QSize& resolution);
    void setup_rendering();
    void synchronize();
    void render();
    void cleanup();

private:
    QQuickWindow* window_ = nullptr;
};

#endif
