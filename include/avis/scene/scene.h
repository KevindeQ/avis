#ifndef AVIS_SCENE_SCENE_H
#define AVIS_SCENE_SCENE_H

#include "avis/scene/components/material.h"
#include "avis/scene/components/mesh.h"

#include <entt/entt.hpp>

#include <generator>
#include <tuple>

namespace avis::scene
{
    class scene
    {
    public:
        template<typename... components_t>
        std::generator<std::tuple<entt::entity, components_t...>> view_entities()
        {
            auto entity_view = entity_registry_.view<components_t...>();
            for (auto result : entity_view.each())
            {
                co_yield result;
            }
        }

    private:
        entt::registry entity_registry_;
    };
} // namespace avis::scene

#endif
