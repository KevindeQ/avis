#ifndef VISUALIZER_VIEWPORT_H
#define VISUALIZER_VIEWPORT_H

#include <QQuickItem>

class viewport : public QQuickItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Viewport)

public:
    explicit viewport(QQuickItem* parent = nullptr);

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
};

#endif
