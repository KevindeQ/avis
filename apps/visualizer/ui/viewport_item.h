#ifndef AVIS_VISUALIZER_UI_VIEWPORT_ITEM_H
#define AVIS_VISUALIZER_UI_VIEWPORT_ITEM_H

// The 3D viewport in QML (docs/design/12-visualizer.md §3, `Viewport`): a QQuickRhiItem whose renderer
// (ui/viewport_renderer) draws the newest frame of the shared frame ring into the item's colour buffer. The item lives
// on the GUI thread; its renderer on Qt's render thread.

#include "avis/graphics/texture.h"

#include <qqmlintegration.h>
#include <QQuickRhiItem>
#include <QTimer>

#include <functional>
#include <optional>

namespace avis::visualizer::rendering
{
    class shared_frame_rings;
}

namespace avis::visualizer::ui
{
    class viewport_item : public QQuickRhiItem
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(Viewport)

    public:
        explicit viewport_item(QQuickItem* parent = nullptr);

        // GUI thread. The rings whose frames the item shows, and where it reports its pixel size (12 §3.3 rule 8);
        // nullptr: none, the item shows its clear colour. The rings must outlive the item's renderer, which the
        // scene graph destroys with the window.
        void set_frame_rings(rendering::shared_frame_rings* rings);

        rendering::shared_frame_rings* frame_rings() const noexcept
        {
            return rings_;
        }

        // Any thread: a function that posts update() to the GUI thread, so the renderer picks the newest frame (12 §3.3
        // rule 5). The frame chain calls it when shared_frame_ring::publish returns true. Safe after the item is gone:
        // the posted call then does nothing. Needs the QCoreApplication alive.
        std::function<void()> make_wake();

        // The colour buffer's pixel size (item size × device pixel ratio), the size the ring's frames are rendered at.
        // Empty until Qt has rendered the item once.
        std::optional<avis::graphics::texture_extent> pixel_size() const;

        // How long a size must hold before it's reported (12 §3.3 rule 8): resizing a window doesn't create a ring per
        // step
        static constexpr int resize_debounce_ms = 100;

    protected:
        QQuickRhiItemRenderer* createRenderer() override;

    private:
        void on_colour_buffer_resized();
        void report_size();

        rendering::shared_frame_rings* rings_ = nullptr;
        QTimer resize_timer_;
        bool size_reported_ = false; // the first size goes out at once, later ones after the debounce
    };
} // namespace avis::visualizer::ui

#endif
