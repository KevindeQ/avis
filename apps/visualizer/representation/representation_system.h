#ifndef AVIS_VISUALIZER_REPRESENTATION_REPRESENTATION_SYSTEM_H
#define AVIS_VISUALIZER_REPRESENTATION_REPRESENTATION_SYSTEM_H

// The scene representation (docs/design/12-visualizer.md §9): step 4 of the tick sequence (08 §9.6), and of every
// presentation republish. It turns the semantic components into helper entities with mesh, material, render_options
// and `represents { owner }`, from a shared library of meshes and materials (shared_assets, material_cache).
//
// Change tracking: the scene's on_construct, on_update and on_destroy signals for display_shape record the entities
// to revisit, so a tick costs what changed, not the size of the scene. Components must be written through
// emplace, replace or patch for that. A change of the representation settings re-runs every entity.
//
// The selection and visibility flags are the simulation loop's: it applies them after this, to each entity and every
// helper that represents it (task 12.6).
//
// Task 12.8 covers display shapes and the grid and axes gizmo; belief geometry, lidar, labels and trails follow in
// 12.9–12.11. Owned by the simulation thread: the one that first calls update(), checked in debug builds.

#include "representation/helper_geometry.h"
#include "representation/material_cache.h"
#include "representation/representation_settings.h"
#include "representation/scene_representation.h"
#include "representation/shape_representation.h"
#include "representation/shared_assets.h"

#include "avis/error.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/scene.h"

#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace avis::visualizer::representation
{
    class representation_system final : public scene_representation
    {
    public:
        // Creates the shared library in the cache, which must outlive this object
        static result<std::unique_ptr<representation_system>> create(scene::asset_cache& assets);

        // Disconnects from a scene it's still attached to; the helpers stay in that scene
        ~representation_system() override;

        representation_system(const representation_system&) = delete;
        representation_system& operator=(const representation_system&) = delete;

        // Attaches to the scene on the first call (every entity with a display shape is then visited); later calls
        // must pass the same scene
        void update(scene::scene& scene, const presentation_state& state) override;

        void detach(scene::scene& scene) noexcept override;

        const shared_assets& shared() const noexcept
        {
            return *shared_;
        }

        const material_cache& materials() const noexcept
        {
            return materials_;
        }

        // An entity's display-shape helpers: faces then edges for a box, one part otherwise
        std::span<const scene::entity> parts(scene::entity owner) const noexcept
        {
            return shapes_.parts(owner);
        }

        // null when off
        scene::entity grid() const noexcept
        {
            return helpers_.grid();
        }

        scene::entity axes_gizmo() const noexcept
        {
            return helpers_.axes_gizmo();
        }

    private:
        representation_system(scene::asset_cache& assets, std::unique_ptr<shared_assets> shared);

        void attach(scene::scene& scene);
        void disconnect() noexcept;

        // display_shape signals: the entity is visited at the next update
        void on_shape_changed(entt::registry& registry, scene::entity entity);

        // Debug check: the calling thread owns this object
        bool check_thread() const noexcept;

        std::unique_ptr<shared_assets> shared_;
        material_cache materials_;
        shape_representation shapes_;
        helper_geometry helpers_;

        scene::scene* scene_ = nullptr; // the attached scene
        std::thread::id owner_thread_;
        std::vector<scene::entity> changed_; // may repeat; deduplicated at update
        std::optional<box_settings> boxes_; // the box settings the parts were built with
    };
} // namespace avis::visualizer::representation

#endif
