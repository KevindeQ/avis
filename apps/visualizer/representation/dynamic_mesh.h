#ifndef AVIS_VISUALIZER_REPRESENTATION_DYNAMIC_MESH_H
#define AVIS_VISUALIZER_REPRESENTATION_DYNAMIC_MESH_H

// The representation's dynamic meshes (docs/design/12-visualizer.md §9, §9.1): belief geometry chunks and lidar sweeps,
// each drawn by one helper entity that holds the mesh. What the geometry budget needs to know about each, and the
// operations its plan executes. Owned by the simulation thread.

#include "representation/geometry_budget.h"
#include "representation/helper_parts.h"

#include "avis/error.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/scene.h"

#include <cstdint>
#include <optional>
#include <string>

namespace avis::visualizer::representation
{
    struct dynamic_mesh
    {
        scene::entity helper = entt::null; // holds the mesh; null while there's none

        // The full mesh, as last built from the source
        bool progressive = false;
        std::uint64_t full_bytes = 0;
        std::uint32_t full_vertices = 0;

        // The version in the cache
        std::uint64_t current_bytes = 0;
        std::uint32_t current_vertices = 0;

        bool changed = true; // the source changed since the current version (or there's none)
        std::optional<scene::mesh_data> pending; // the full new version, while a plan may still use it

        bool exists(const scene::scene& scene) const noexcept
        {
            return helper != entt::null && scene.alive(helper);
        }

        // Records the full mesh's size, and keeps it for the next plan
        void set_pending(scene::mesh_data data);

        budget_item item(
            const scene::scene& scene, budget_kind kind, std::uint64_t sensor = 0, std::int64_t age = 0) const noexcept;
    };

    // The seed of every progressive shuffle: the same source always gives the same order (tests, screenshots)
    inline constexpr std::uint64_t progressive_seed = 0x5eed'a715'0000'0001ull;

    // Puts the vertices of a non-indexed mesh in a random order, the same for the same seed and vertex count on every
    // platform (a Fisher–Yates shuffle on splitmix64), and marks it progressive
    void shuffle_progressive(scene::mesh_data& data, std::uint64_t seed = progressive_seed);

    // Keeps the first `vertices` vertices of a progressive mesh (a uniform subsample), in its one primitive
    void truncate_progressive(scene::mesh_data& data, std::uint32_t vertices);

    // Creates or updates the mesh from `data` cut to `vertices` (progressive meshes only), and the helper that draws it
    // when there's none: named "<owner name> · <label>", under `parent`, representing `owner`. The specification's mesh
    // is ignored. On failure the previous version stays.
    result<void> build_dynamic_mesh(
        scene::scene& scene,
        dynamic_mesh& mesh,
        scene::mesh_data data,
        std::uint32_t vertices,
        const part_specification& part,
        scene::entity parent,
        scene::entity owner,
        const std::string& label);

    // Destroys the helper and its mesh; the source still counts as changed
    void release_dynamic_mesh(scene::scene& scene, dynamic_mesh& mesh);
} // namespace avis::visualizer::representation

#endif
