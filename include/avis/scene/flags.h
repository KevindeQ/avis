#ifndef AVIS_SCENE_FLAGS_H
#define AVIS_SCENE_FLAGS_H

// The enums the components share with the snapshot and the renderer (docs/design/08-scene-management.md §9.2, §6.1),
// in avis::scene so those can use them without the components. entity_category is in types.h.

#include "avis/enum_flags.h"

#include <cstdint>

namespace avis::scene
{
    // Per render instance (08 §6.1, 11 §3.1)
    enum class instance_flags : std::uint8_t
    {
        visible = 1 << 0,
        casts_shadows = 1 << 1,
        selected = 1 << 2,
        pickable = 1 << 3,
        screen_aligned = 1 << 4 // the mesh is in pixels, facing the camera (08 §8.1)
    };

    enum class draw_layer : std::uint8_t
    {
        world,
        overlay,
        overlay_on_top
    };

    // entity_update's flags (02 §5.8.1)
    enum class driver_entity_flags : std::uint16_t
    {
        primary = 1 << 0, // the vehicle of the vehicle model, the follow camera's default target
        pickable = 1 << 1,
        hidden_by_default = 1 << 2
    };

    enum class light_kind : std::uint8_t
    {
        directional,
        point,
        spot
    };

    // The UI's visibility toggles per class of entity (08 §3, visibility): one bit per class, assigned by the
    // Visualizer. An entity is shown when one of its bits is enabled.
    using render_layer_mask = std::uint32_t;
    inline constexpr render_layer_mask all_render_layers = ~render_layer_mask{ 0 };
} // namespace avis::scene

ALLOW_FLAGS_FOR_ENUM(avis::scene::instance_flags)
ALLOW_FLAGS_FOR_ENUM(avis::scene::driver_entity_flags)

namespace avis::scene
{
    // render_options' default (08 §3): visible, casts shadows, pickable
    inline constexpr avis::flags<instance_flags> default_instance_flags =
        instance_flags::visible | instance_flags::casts_shadows | instance_flags::pickable;
} // namespace avis::scene

#endif
