#include "ui/simulation_bridge.h"

#include <QMetaObject>

#include <iterator>
#include <utility>

namespace avis::visualizer::ui
{
    simulation_bridge::simulation_bridge(QObject* const parent) : QObject(parent) {}

    void simulation_bridge::tree_changed(std::vector<scene::tree_change> changes)
    {
        QMetaObject::invokeMethod(
            this,
            [this, changes = std::move(changes)]() mutable
            {
                const bool was_empty = tree_changes_.empty();
                tree_changes_.insert(
                    tree_changes_.end(),
                    std::make_move_iterator(changes.begin()),
                    std::make_move_iterator(changes.end()));
                if (was_empty)
                {
                    Q_EMIT tree_changes_available(); // once until the next take_tree_changes
                }
            },
            Qt::QueuedConnection);
    }

    void simulation_bridge::selected_entity_changed(std::optional<scene::reflected_entity> entity)
    {
        QMetaObject::invokeMethod(
            this,
            [this, entity = std::move(entity)]() mutable
            {
                selected_ = std::move(entity);
                Q_EMIT selected_entity_updated();
            },
            Qt::QueuedConnection);
    }

    void simulation_bridge::statistics_changed(
        const scene::scene_statistics& statistics, const representation::representation_statistics& representation)
    {
        QMetaObject::invokeMethod(
            this,
            [this, statistics, representation]
            {
                statistics_ = statistics;
                representation_statistics_ = representation;
                Q_EMIT statistics_updated();
            },
            Qt::QueuedConnection);
    }

    void simulation_bridge::follow_target_changed(const simulation::follow_target& target)
    {
        QMetaObject::invokeMethod(
            this,
            [this, target]
            {
                follow_ = target;
                Q_EMIT follow_target_updated();
            },
            Qt::QueuedConnection);
    }

    std::vector<scene::tree_change> simulation_bridge::take_tree_changes()
    {
        return std::exchange(tree_changes_, {});
    }
} // namespace avis::visualizer::ui
