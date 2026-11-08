#ifndef AVIS_VISUALIZER_REPRESENTATION_SHAPE_REPRESENTATION_H
#define AVIS_VISUALIZER_REPRESENTATION_SHAPE_REPRESENTATION_H

// Display shapes to helper children (docs/design/12-visualizer.md §9's table):
// - box: the unit cube in `world`, translucent in the entity's colour and opacity, and the cube's edges in `overlay`,
//   unlit, with representation.boxes.edge_width
// - vehicle: the proxy in `world`, with a ghost material when the opacity is below 1
// - axes: the shared axes lines in `overlay`, scaled to the extent
// - frustum: lines in `overlay` built from the angles and the range, one mesh per entity
//
// Each part is a helper child of the entity with `represents { entity }`, so picking it selects the entity and the
// scene tree hides it. The extent is the part's scale, never the entity's (08 §4.3). Owned by the simulation thread.

#include "representation/material_cache.h"
#include "representation/representation_settings.h"
#include "representation/shared_assets.h"

#include "avis/scene/components/driver.h"
#include "avis/scene/scene.h"

#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace avis::visualizer::representation
{
    class shape_representation
    {
    public:
        // Both must outlive this object
        shape_representation(const shared_assets& shared, material_cache& materials) :
            shared_{ shared },
            materials_{ materials }
        {}

        // Brings the entity's parts in line with its display_shape. An entity without one, or with shape none, loses
        // them; a dead entity is forgotten (its parts went with its subtree).
        void update(scene::scene& scene, scene::entity owner, const representation_settings& settings);

        // Forgets every entity, without touching the scene (it's about to be destroyed)
        void clear() noexcept
        {
            states_.clear();
        }

        // The entity's parts, in the order of the table above (faces, then edges); empty when it has none
        std::span<const scene::entity> parts(scene::entity owner) const noexcept;

        std::size_t represented() const noexcept
        {
            return states_.size();
        }

    private:
        struct part_specification
        {
            const char* name = "";
            scene::mesh_handle mesh;
            std::vector<scene::material_handle> materials;
            scene::draw_layer layer = scene::draw_layer::world;
            avis::flags<scene::instance_flags> flags = scene::default_instance_flags;
            scene::transform local;
        };

        struct frustum_parameters
        {
            float horizontal_fov = 0.0f;
            float vertical_fov = 0.0f;
            float range = 0.0f;

            friend bool operator==(const frustum_parameters&, const frustum_parameters&) = default;
        };

        struct entity_state
        {
            scene::components::shape_kind kind = scene::components::shape_kind::none;
            std::vector<scene::entity> parts;
            std::optional<frustum_parameters> frustum; // the parameters the frustum mesh was built with
        };

        // The parts the shape needs. A frustum mesh created here comes back in `created`, with this object's
        // reference, to release once the part holds it.
        result<std::vector<part_specification>> specify(
            scene::scene& scene,
            const scene::components::display_shape& shape,
            const representation_settings& settings,
            entity_state& state,
            scene::mesh_handle& created);

        static void destroy_parts(scene::scene& scene, entity_state& state);

        // Writes only what differs, so unchanged parts raise no update signals
        static void apply(scene::scene& scene, scene::entity part, const part_specification& specification);

        const shared_assets& shared_;
        material_cache& materials_;
        std::unordered_map<scene::entity, entity_state> states_;
    };
} // namespace avis::visualizer::representation

#endif
