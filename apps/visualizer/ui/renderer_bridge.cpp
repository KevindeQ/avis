#include "ui/renderer_bridge.h"

#include <QMetaObject>

#include <iterator>
#include <utility>

namespace avis::visualizer::ui
{
    namespace
    {
        // Appends `added` to `pending`; true when `pending` was empty, so the caller emits its signal once
        template<class T>
        bool append(std::vector<T>& pending, std::vector<T>& added)
        {
            const bool was_empty = pending.empty();
            pending.insert(pending.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
            return was_empty;
        }
    } // namespace

    renderer_bridge::renderer_bridge(QObject* const parent) : QObject(parent) {}

    void renderer_bridge::debug_information_changed(rendering::frame_debug_information information)
    {
        QMetaObject::invokeMethod(
            this,
            [this, information = std::move(information)]() mutable
            {
                debug_information_ = std::move(information);
                Q_EMIT debug_information_updated();
            },
            Qt::QueuedConnection);
    }

    void renderer_bridge::picks_completed(std::vector<rendering::pick_result> results)
    {
        QMetaObject::invokeMethod(
            this,
            [this, results = std::move(results)]() mutable
            {
                if (append(picks_, results))
                {
                    Q_EMIT picks_available();
                }
            },
            Qt::QueuedConnection);
    }

    void renderer_bridge::inspections_completed(std::vector<rendering::inspector_result> results)
    {
        QMetaObject::invokeMethod(
            this,
            [this, results = std::move(results)]() mutable
            {
                if (append(inspections_, results))
                {
                    Q_EMIT inspections_available();
                }
            },
            Qt::QueuedConnection);
    }

    void renderer_bridge::screenshots_completed(std::vector<rendering::screenshot_result> results)
    {
        QMetaObject::invokeMethod(
            this,
            [this, results = std::move(results)]() mutable
            {
                if (append(screenshots_, results))
                {
                    Q_EMIT screenshots_available();
                }
            },
            Qt::QueuedConnection);
    }

    void renderer_bridge::fatal_error(avis::error error)
    {
        QMetaObject::invokeMethod(
            this,
            [this, message = QString::fromStdString(error.message)]() { Q_EMIT fatal_error_occurred(message); },
            Qt::QueuedConnection);
    }

    std::vector<rendering::pick_result> renderer_bridge::take_picks()
    {
        return std::exchange(picks_, {});
    }

    std::vector<rendering::inspector_result> renderer_bridge::take_inspections()
    {
        return std::exchange(inspections_, {});
    }

    std::vector<rendering::screenshot_result> renderer_bridge::take_screenshots()
    {
        return std::exchange(screenshots_, {});
    }
} // namespace avis::visualizer::ui
