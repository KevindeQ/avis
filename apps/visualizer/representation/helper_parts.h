#ifndef AVIS_VISUALIZER_REPRESENTATION_HELPER_PARTS_H
#define AVIS_VISUALIZER_REPRESENTATION_HELPER_PARTS_H

// What every representation does with its helper entities (docs/design/12-visualizer.md §9): keep a part's mesh,
// materials, layer, flags and transform in line with what it should draw, writing only what differs so unchanged
// parts raise no update signals. Owned by the simulation thread, like the scene.

#include "avis/enum_flags.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/flags.h"
#include "avis/scene/scene.h"
#include "avis/scene/types.h"

#include <vector>

namespace avis::visualizer::representation
{
    struct part_specification
    {
        const char* name = ""; // after "<owner> · " in the part's name, when it's created
        scene::mesh_handle mesh;
        std::vector<scene::material_handle> materials;
        scene::draw_layer layer = scene::draw_layer::world;
        avis::flags<scene::instance_flags> flags = scene::default_instance_flags;
        scene::transform local;
    };

    // Lines, translucent surfaces and data overlays: no shadows, nothing screen-aligned
    inline constexpr avis::flags<scene::instance_flags> overlay_flags =
        scene::instance_flags::visible | scene::instance_flags::pickable;

    // Writes the specification's mesh, materials, render options and transform where they differ. The selected flag
    // belongs to the simulation loop, which sets it after the representation, so it's kept.
    void apply_part(scene::scene& scene, scene::entity part, const part_specification& specification);

    bool same_transform(const scene::transform& a, const scene::transform& b) noexcept;

    // The `helpers` group (08 §4.2): the root helper named helpers_group_name. `cached` is checked first and updated;
    // null when there's no group (helpers then become roots).
    scene::entity find_helpers_group(scene::scene& scene, scene::entity& cached);
} // namespace avis::visualizer::representation

#endif
