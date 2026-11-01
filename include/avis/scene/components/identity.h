#ifndef AVIS_SCENE_COMPONENTS_IDENTITY_H
#define AVIS_SCENE_COMPONENTS_IDENTITY_H

// Identity and helper ownership (docs/design/08-scene-management.md §3, §9.2)

#include "avis/scene/types.h"

#include <entt/entity/entity.hpp>

#include <string>

namespace avis::scene::components
{
    // Owned by scene: written only through scene calls (08 §9.2)
    struct identity
    {
        object_id id = 0; // the Driver's entity_id, or one in the helper range (types.h)
        std::string name;
        entity_category category = entity_category::helper; // set for every entity; helpers get helper
    };

    // A helper the representation created for another entity (box edges, a label, a trail). Picking it selects the
    // owner (scene::selection_target), and the scene tree hides it.
    struct represents
    {
        entity owner = entt::null;
    };
} // namespace avis::scene::components

#endif
