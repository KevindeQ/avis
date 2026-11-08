#ifndef AVIS_VISUALIZER_REPRESENTATION_LIDAR_REPRESENTATION_H
#define AVIS_VISUALIZER_REPRESENTATION_LIDAR_REPRESENTATION_H

// Lidar sweeps to point meshes (docs/design/12-visualizer.md §9's table): one progressive point mesh per kept sweep,
// drawn by a helper under the `helpers` group at the sweep's sensor pose, so a sweep stays where it was captured while
// the lidar moves on. Each helper represents the lidar entity (picking a point selects the lidar).
//
// - Points are lit, face the camera (no normals) and don't cast shadows, at visualization.lidar.point_size.
// - Colours follow visualization.lidar.color_mode: intensity or height (world z) through one colour map, ring through
//   a cyclic palette, or uniform. A sweep without the channel its mode needs is drawn uniform.
// - representation.lidar.maximum_points_per_sweep keeps a random prefix: the shuffle comes first.
// - A new colour mode (or maximum) rebuilds every kept sweep: new versions of the same meshes. A new point size only
//   swaps the material.
//
// Building and releasing go through the geometry budget's plan, like geometry_representation. Owned by the
// simulation thread.

#include "representation/dynamic_mesh.h"
#include "representation/geometry_budget.h"
#include "representation/material_cache.h"
#include "representation/presentation_state.h"
#include "representation/representation_settings.h"

#include "avis/error.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/scene.h"
#include "avis/simulation/time.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace avis::visualizer::representation
{
    // The colour map of the intensity and height modes, t in [0, 1] (clamped), sRGB
    std::array<std::uint8_t, 4> lidar_color_map(float t) noexcept;

    // The height mode's range of world z, in metres
    inline constexpr float lidar_height_minimum = -1.0f;
    inline constexpr float lidar_height_maximum = 4.0f;

    // The colour of uniform points, sRGB
    inline constexpr std::array<std::uint8_t, 4> lidar_uniform_color{ 190, 200, 215, 255 };

    // The sweep's mesh at full size: shuffled, then cut to `maximum_points`. Its colours follow the mode, or it has
    // none (uniform).
    result<scene::mesh_data> build_sweep_mesh(
        const scene::components::lidar_sweep& sweep, lidar_color_mode mode, std::uint32_t maximum_points);

    class lidar_representation
    {
    public:
        explicit lidar_representation(material_cache& materials) : materials_{ materials } {}

        // A new colour mode or maximum marks every sweep changed; a new point size swaps the helpers' materials. True
        // when the budget has something to plan.
        bool restyle(scene::scene& scene, const lidar_view& view, const lidar_settings& settings);

        // Brings the lidar's sweep records in line with its lidar_points: new sweeps get their full mesh built for the
        // next plan, gone ones lose their helper. A dead entity loses its helpers and is forgotten. True when something
        // changed for the budget.
        bool update(scene::scene& scene, scene::entity owner);

        // Appends one budget item per kept sweep, and remembers the order for execute()
        void collect(const scene::scene& scene, std::vector<budget_item>& items);

        void execute(scene::scene& scene, std::size_t index, const budget_decision& decision);

        // After a plan: drops the full mesh data kept for it
        void finish() noexcept;

        // The vertex and index bytes of the current versions
        std::uint64_t used_bytes(const scene::scene& scene) const noexcept;

        void clear() noexcept
        {
            owners_.clear();
            collected_.clear();
            group_ = entt::null;
        }

        // The lidar's sweep records, oldest first; a dropped or skipped sweep has no helper
        std::size_t sweep_count(scene::entity owner) const noexcept;
        const dynamic_mesh* sweep_mesh(scene::entity owner, std::size_t index) const noexcept;

    private:
        struct sweep_state
        {
            avis::simulation::simulation_duration capture_time{ 0 };
            dynamic_mesh mesh;
        };

        using owner_state = std::deque<sweep_state>;

        const scene::components::lidar_sweep* find_sweep(
            const scene::scene& scene, scene::entity owner, avis::simulation::simulation_duration time) const;

        result<scene::material_handle> material_for(bool colored);

        result<void> build(scene::scene& scene, scene::entity owner, sweep_state& state, std::uint32_t vertices);

        material_cache& materials_;
        lidar_view view_;
        lidar_settings settings_;
        std::unordered_map<scene::entity, owner_state> owners_;
        std::vector<std::pair<scene::entity, avis::simulation::simulation_duration>> collected_;
        scene::entity group_ = entt::null; // the helpers group, cached
    };
} // namespace avis::visualizer::representation

#endif
