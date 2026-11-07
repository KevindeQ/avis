#include "ui/viewport_item.h"

#include "rendering/shared_frame_ring.h"
#include "ui/viewport_renderer.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <cstdint>

namespace avis::visualizer::ui
{
    viewport_item::viewport_item(QQuickItem* parent) : QQuickRhiItem(parent)
    {
        resize_timer_.setSingleShot(true);
        resize_timer_.setInterval(resize_debounce_ms);
        QObject::connect(&resize_timer_, &QTimer::timeout, this, &viewport_item::report_size);
        // Emitted on the GUI thread once the render thread has resized the colour buffer, for a new item size or
        // device pixel ratio alike
        QObject::connect(
            this, &QQuickRhiItem::effectiveColorBufferSizeChanged, this, &viewport_item::on_colour_buffer_resized);
    }

    void viewport_item::set_frame_rings(rendering::shared_frame_rings* const rings)
    {
        rings_ = rings;
        size_reported_ = false;
        report_size();
        update(); // the renderer takes the rings in synchronize()
    }

    std::function<void()> viewport_item::make_wake()
    {
        // The pointer is only read on the GUI thread, inside the posted call. The application object is the context,
        // so the call is delivered even when the item is gone.
        return [item = QPointer<viewport_item>{ this }]()
        {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [item]()
                {
                    if (item)
                    {
                        item->update();
                    }
                },
                Qt::QueuedConnection);
        };
    }

    std::optional<avis::graphics::texture_extent> viewport_item::pixel_size() const
    {
        const QSize size = effectiveColorBufferSize();
        if (size.width() <= 0 || size.height() <= 0)
        {
            return std::nullopt;
        }
        return avis::graphics::texture_extent{ .width = static_cast<std::uint32_t>(size.width()),
                                               .height = static_cast<std::uint32_t>(size.height()) };
    }

    QQuickRhiItemRenderer* viewport_item::createRenderer()
    {
        return make_viewport_renderer();
    }

    void viewport_item::on_colour_buffer_resized()
    {
        if (!size_reported_)
        {
            report_size();
            return;
        }
        resize_timer_.start(); // restarts: only a size that holds for the debounce is reported
    }

    void viewport_item::report_size()
    {
        const std::optional<avis::graphics::texture_extent> size = pixel_size();
        if (rings_ == nullptr || !size)
        {
            return;
        }
        rings_->request_resize(*size);
        size_reported_ = true;
    }
} // namespace avis::visualizer::ui
