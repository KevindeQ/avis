#ifndef AVIS_SCENE_COMPONENTS_SPATIAL_H
#define AVIS_SCENE_COMPONENTS_SPATIAL_H

// The link to the scene graph and the previous tick's world transform (docs/design/08-scene-management.md §3, §6.3).
// Both are owned by scene: written only through scene calls (08 §9.2).

#include "avis/scene/types.h"

#include <Eigen/Geometry>

namespace avis::scene::components
{
    struct node_reference
    {
        node_handle node;
    };

    // The node's world transform at the end of the previous tick, or the current one after a jump (08 §4.1). An entity
    // created this tick has none yet (08 §6.3).
    struct previous_world
    {
        Eigen::Affine3f world = Eigen::Affine3f::Identity();
    };
} // namespace avis::scene::components

#endif
