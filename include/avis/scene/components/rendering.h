#ifndef AVIS_SCENE_COMPONENTS_RENDERING_H
#define AVIS_SCENE_COMPONENTS_RENDERING_H

// What an entity draws and how (docs/design/08-scene-management.md §3, §6.3, §9.2): the mesh and its materials, render
// settings, visibility toggles and lights. Only these reach the snapshot; the semantic components (driver.h) are
// turned into them by the Visualizer's scene representation.

#include "avis/enum_flags.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/flags.h"
#include "avis/scene/types.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace avis::scene::components
{
    // A range of an asset's primitives. scene retains the handle while the component holds it (08 §2, §9.5).
    struct mesh
    {
        // primitive_count's "through the last primitive", whatever the current version has
        static constexpr std::uint32_t all_primitives = std::numeric_limits<std::uint32_t>::max();

        mesh_handle asset;
        std::uint32_t primitive_first = 0;
        std::uint32_t primitive_count = all_primitives;
    };

    // One material per primitive of the mesh's range; the last one repeats when there are fewer (10.11). scene
    // retains the handles while the component holds them.
    struct material
    {
        std::vector<material_handle> materials;
    };

    // Optional, for an entity with mesh and material. Without it, the defaults below apply.
    struct render_options
    {
        draw_layer layer = draw_layer::world;
        std::uint8_t layer_priority = 0;
        avis::flags<instance_flags> flags = default_instance_flags;
        float maximum_view_distance = 0.0f; // metres from the camera; 0 = unlimited (08 §8.1)
    };

    // The UI's toggles per entity or class
    struct visibility
    {
        bool visible = true;
        render_layer_mask layers = all_render_layers;
    };

    // −Z of the entity's node is the light direction (08 §9.8)
    struct light
    {
        light_kind kind = light_kind::directional;
        color light_color;
        float intensity = 1.0f; // lux (directional), candela (point, spot)
        float range = 0.0f; // metres; 0 = unlimited
        float inner_cone_angle = 0.0f; // radians, spot
        float outer_cone_angle = 0.0f; // radians, spot
        bool casts_shadows = false;
    };
} // namespace avis::scene::components

#endif
