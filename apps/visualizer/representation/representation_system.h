#ifndef AVIS_VISUALIZER_REPRESENTATION_REPRESENTATION_SYSTEM_H
#define AVIS_VISUALIZER_REPRESENTATION_REPRESENTATION_SYSTEM_H

// The scene representation (docs/design/12-visualizer.md §9): step 4 of the tick sequence (08 §9.6), and of every
// presentation republish. It turns the semantic components into helper entities with mesh, material, render_options
// and `represents { owner }`, from a shared library of meshes and materials (shared_assets, material_cache).
//
// Change tracking: the scene's on_construct, on_update and on_destroy signals for display_shape, belief_geometry and
// lidar_points record the entities to revisit, so a tick costs what changed, not the size of the scene. Components
// must be written through emplace, replace or patch for that. A change of the representation settings re-runs the
// entities they affect.
//
// The dynamic meshes (belief geometry chunks, lidar sweeps) share the geometry budget (12 §9.1): whenever one of them
// changes, the budget plans them all, and its reduction level comes back through statistics().
//
// The selection and visibility flags are the simulation loop's: it applies them after this, to each entity and every
// helper that represents it (task 12.6).
//
// Labels, trails and staleness follow in 12.10–12.11. Owned by the simulation thread: the one that first calls
// update(), checked in debug builds.

#include "representation/geometry_budget.h"
#include "representation/geometry_representation.h"
#include "representation/helper_geometry.h"
#include "representation/lidar_representation.h"
#include "representation/material_cache.h"
#include "representation/representation_settings.h"
#include "representation/scene_representation.h"
#include "representation/shape_representation.h"
#include "representation/shared_assets.h"

#include "avis/error.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/scene.h"

#include <chrono>
#include <cstdint>
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

        // Attaches to the scene on the first call (every entity with a semantic component is then visited); later
        // calls must pass the same scene
        void update(scene::scene& scene, const presentation_state& state) override;

        void detach(scene::scene& scene) noexcept override;

        representation_statistics statistics() const noexcept override
        {
            return { .geometry = reduction_ };
        }

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

        const geometry_representation& geometry() const noexcept
        {
            return geometry_;
        }

        const lidar_representation& lidar() const noexcept
        {
            return lidar_;
        }

        // The budget's last plan
        const geometry_reduction& reduction() const noexcept
        {
            return reduction_;
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

        // Signals: the entity is visited at the next update
        void on_shape_changed(entt::registry& registry, scene::entity entity);
        void on_geometry_changed(entt::registry& registry, scene::entity entity);
        void on_lidar_changed(entt::registry& registry, scene::entity entity);

        // Plans every dynamic mesh against the budget and carries the plan out
        void plan_budget(scene::scene& scene, std::uint64_t budget_bytes);

        // Debug check: the calling thread owns this object
        bool check_thread() const noexcept;

        // Each entity once, in a stable order; keeps the vector's capacity
        static std::vector<scene::entity> take_unique(std::vector<scene::entity>& changed);

        std::unique_ptr<shared_assets> shared_;
        material_cache materials_;
        shape_representation shapes_;
        geometry_representation geometry_;
        lidar_representation lidar_;
        helper_geometry helpers_;

        scene::scene* scene_ = nullptr; // the attached scene
        std::thread::id owner_thread_;

        // May repeat; deduplicated at update
        std::vector<scene::entity> shapes_changed_;
        std::vector<scene::entity> geometry_changed_;
        std::vector<scene::entity> lidar_changed_;

        // The settings the helpers were built with
        std::optional<box_settings> boxes_;
        std::optional<geometry_settings> budget_;

        geometry_reduction reduction_;
        std::vector<budget_item> items_; // reused by every plan
        std::chrono::steady_clock::time_point last_skip_warning_{};
    };
} // namespace avis::visualizer::representation

#endif
