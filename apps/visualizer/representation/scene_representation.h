#ifndef AVIS_VISUALIZER_REPRESENTATION_SCENE_REPRESENTATION_H
#define AVIS_VISUALIZER_REPRESENTATION_SCENE_REPRESENTATION_H

// Step 4 of the tick sequence (docs/design/08-scene-management.md §9.6, 12 §9): what turns the scene's semantic
// components into meshes and materials. The simulation thread calls it on every tick and every presentation republish.
// Task 12.8's representation_system implements it; tests use fakes.

namespace avis::scene
{
    class scene;
} // namespace avis::scene

namespace avis::visualizer::representation
{
    struct presentation_state;

    class scene_representation
    {
    public:
        virtual ~scene_representation() = default;

        // On the simulation thread, after the tick's transforms (step 2) or, for a republish, with the transforms of
        // the last tick. May create, change and destroy helper entities and call set_local on them; the caller
        // updates the transforms after it (step 5).
        virtual void update(scene::scene& scene, const presentation_state& state) = 0;

    protected:
        scene_representation() = default;
        scene_representation(const scene_representation&) = default;
        scene_representation& operator=(const scene_representation&) = default;
    };
} // namespace avis::visualizer::representation

#endif
