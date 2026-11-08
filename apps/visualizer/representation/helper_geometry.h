#ifndef AVIS_VISUALIZER_REPRESENTATION_HELPER_GEOMETRY_H
#define AVIS_VISUALIZER_REPRESENTATION_HELPER_GEOMETRY_H

// The Visualizer's helpers (docs/design/12-visualizer.md §9): the world grid in `overlay` and the axes gizmo at the
// origin in `overlay_on_top`, as entities under the `helpers` group, created or destroyed by the View menu's toggles
// (presentation_state::grid, axes_gizmo). Neither is pickable or casts shadows. The sun isn't a helper: the
// environment carries it (scene::set_environment). Owned by the simulation thread.

#include "representation/presentation_state.h"
#include "representation/shared_assets.h"

#include "avis/scene/scene.h"

namespace avis::visualizer::representation
{
    // The gizmo's axes are this long, in metres
    inline constexpr float axes_gizmo_length = 2.0f;

    class helper_geometry
    {
    public:
        explicit helper_geometry(const shared_assets& shared) : shared_{ shared } {}

        // Creates or destroys the grid and the gizmo to match the toggles
        void update(scene::scene& scene, const presentation_state& state);

        // Forgets the entities, without touching the scene (it's about to be destroyed)
        void clear() noexcept
        {
            grid_ = entt::null;
            gizmo_ = entt::null;
            group_ = entt::null;
        }

        // null when off
        scene::entity grid() const noexcept
        {
            return grid_;
        }

        scene::entity axes_gizmo() const noexcept
        {
            return gizmo_;
        }

    private:
        void ensure(
            scene::scene& scene,
            scene::entity& slot,
            bool wanted,
            const char* name,
            scene::mesh_handle mesh,
            scene::material_handle material,
            scene::draw_layer layer,
            float scale);

        const shared_assets& shared_;
        scene::entity grid_ = entt::null;
        scene::entity gizmo_ = entt::null;
        scene::entity group_ = entt::null;
    };
} // namespace avis::visualizer::representation

#endif
