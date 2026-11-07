#include "representation/presentation_state.h"

#include <cstddef>

namespace avis::visualizer::representation
{
    presentation_state presentation_state::from_settings(
        const representation_settings& settings, const scene::environment_settings& environment)
    {
        presentation_state state;
        state.representation = settings;
        state.environment = environment;
        state.stale = settings.stale.enabled;
        for (std::size_t category = 0; category < scene::entity_category_count; ++category)
        {
            state.labels[category] = settings.labels.enabled && !settings.labels.templates[category].empty();
            state.trails[category] = settings.trails[category].duration_seconds > 0.0f;
        }
        return state;
    }

    bool presentation_state::visible(const scene::object_id id, const scene::entity_category category) const noexcept
    {
        if (!category_visible[static_cast<std::size_t>(category)])
        {
            return false;
        }
        const auto found = entity_visible.find(id);
        return found == entity_visible.end() || found->second;
    }

    bool presentation_state::trail(const scene::object_id id, const scene::entity_category category) const noexcept
    {
        const auto found = entity_trails.find(id);
        return found != entity_trails.end() ? found->second : trails[static_cast<std::size_t>(category)];
    }

    per_category<bool> presentation_state::make_all(const bool value) noexcept
    {
        per_category<bool> all;
        all.fill(value);
        return all;
    }
} // namespace avis::visualizer::representation
