#include "representation/helper_parts.h"

#include "representation/scene_representation.h"

#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"

#include <string_view>

namespace avis::visualizer::representation
{
    void apply_part(scene::scene& scene, const scene::entity part, const part_specification& specification)
    {
        using namespace avis::scene::components;
        entt::registry& registry = scene.registry();

        if (!same_transform(scene.local(part), specification.local))
        {
            scene.set_local(part, specification.local);
        }

        if (const mesh* current = registry.try_get<mesh>(part);
            current == nullptr || current->asset != specification.mesh)
        {
            registry.emplace_or_replace<mesh>(part, mesh{ .asset = specification.mesh });
        }

        if (const material* current = registry.try_get<material>(part);
            current == nullptr || current->materials != specification.materials)
        {
            registry.emplace_or_replace<material>(part, material{ .materials = specification.materials });
        }

        render_options wanted{ .layer = specification.layer, .flags = specification.flags };
        if (const render_options* current = registry.try_get<render_options>(part))
        {
            if (current->flags.count(scene::instance_flags::selected) != 0)
            {
                wanted.flags |= scene::instance_flags::selected;
            }
            if (current->layer == wanted.layer && current->flags == wanted.flags &&
                current->layer_priority == wanted.layer_priority &&
                current->maximum_view_distance == wanted.maximum_view_distance)
            {
                return;
            }
        }
        registry.emplace_or_replace<render_options>(part, wanted);
    }

    bool same_transform(const scene::transform& a, const scene::transform& b) noexcept
    {
        return a.position == b.position && a.rotation.coeffs() == b.rotation.coeffs() && a.scale == b.scale;
    }

    scene::entity find_helpers_group(scene::scene& scene, scene::entity& cached)
    {
        using namespace avis::scene::components;
        if (cached != entt::null && scene.alive(cached))
        {
            return cached;
        }
        cached = entt::null;
        for (const auto [candidate, who] : scene.registry().view<identity>().each())
        {
            if (who.category == scene::entity_category::helper && std::string_view{ who.name } == helpers_group_name &&
                !scene.registry().all_of<represents>(candidate))
            {
                cached = candidate;
                break;
            }
        }
        return cached;
    }
} // namespace avis::visualizer::representation
