#ifndef AVIS_VISUALIZER_REPRESENTATION_SCENE_REPRESENTATION_H
#define AVIS_VISUALIZER_REPRESENTATION_SCENE_REPRESENTATION_H

// Step 4 of the tick sequence (docs/design/08-scene-management.md §9.6, 12 §9): what turns the scene's semantic
// components into meshes and materials. The simulation thread calls it on every tick and every presentation republish.
// Task 12.8's representation_system implements it; tests use fakes.

#include "representation/geometry_budget.h"

#include "avis/scene/scene.h"
#include "avis/scene/types.h"

namespace avis::visualizer::representation
{
    struct presentation_state;

    // The root entity the Visualizer's own helpers (grid, axes gizmo, trails) live under (08 §4.2). The simulation
    // loop creates it; the representation finds it by this name.
    inline constexpr const char* helpers_group_name = "helpers";

    // What the representation reports with the scene statistics (08 §8.5, 12 §9.1)
    struct representation_statistics
    {
        geometry_reduction geometry; // the budget's reduction level

        friend bool operator==(const representation_statistics&, const representation_statistics&) = default;
    };

    class scene_representation
    {
    public:
        virtual ~scene_representation() = default;

        // On the simulation thread, after the tick's transforms (step 2) or, for a republish, with the transforms of
        // the last tick. May create, change and destroy helper entities and call set_local on them; the caller
        // updates the transforms after it (step 5).
        virtual void update(scene::scene& scene, const presentation_state& state) = 0;

        // The scene is about to be destroyed: drop whatever ties this object to it (signal connections). The scene
        // destroys the helpers itself. update() isn't called after this.
        virtual void detach(scene::scene& scene) noexcept
        {
            static_cast<void>(scene);
        }

        // As of the last update()
        virtual representation_statistics statistics() const noexcept
        {
            return {};
        }

    protected:
        scene_representation() = default;
        scene_representation(const scene_representation&) = default;
        scene_representation& operator=(const scene_representation&) = default;
    };
} // namespace avis::visualizer::representation

#endif
