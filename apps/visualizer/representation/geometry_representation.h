#ifndef AVIS_VISUALIZER_REPRESENTATION_GEOMETRY_REPRESENTATION_H
#define AVIS_VISUALIZER_REPRESENTATION_GEOMETRY_REPRESENTATION_H

// Belief geometry to meshes (docs/design/12-visualizer.md §9's table): one mesh per belief_geometry chunk, drawn by a
// helper child of the entity at the chunk's origin, with a material from the chunk's style (colour, opacity, lit or
// unlit, line width, point size) in the style's layer.
//
// - Topologies: triangles, lines and points as they are; a line strip becomes a line list. Lit triangles without
//   normals get smooth ones. With vertex colours, those replace the style's colour (its alpha still applies).
// - Points chunks are progressive: their vertices are shuffled once, with a fixed seed, when the mesh is built.
// - Changes are found by an XXH3 hash of each chunk, so a geometry_update that rewrites one chunk rebuilds only that
//   one. A removed chunk releases its mesh.
//
// Building and releasing go through the geometry budget's plan (geometry_budget.h): update() records what changed and
// builds the full mesh data, collect() reports each chunk to the budget, execute() carries out its decision. Owned by
// the simulation thread.

#include "representation/dynamic_mesh.h"
#include "representation/geometry_budget.h"
#include "representation/material_cache.h"

#include "avis/error.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace avis::visualizer::representation
{
    // The chunk's mesh at full size: progressive and shuffled for points. invalid_mesh when its streams don't match.
    result<scene::mesh_data> build_chunk_mesh(const scene::components::geometry_chunk& chunk);

    // The content hash change detection uses
    std::uint64_t hash_chunk(const scene::components::geometry_chunk& chunk) noexcept;

    class geometry_representation
    {
    public:
        explicit geometry_representation(material_cache& materials) : materials_{ materials } {}

        // Brings the entity's chunk records in line with its belief_geometry. A dead entity is forgotten (its helpers
        // were its children). True when something changed for the budget.
        bool update(scene::scene& scene, scene::entity owner);

        // Appends one budget item per chunk, and remembers the order for execute()
        void collect(const scene::scene& scene, std::vector<budget_item>& items);

        // Carries out the budget's decision for the collect()ed item `index` (0 = this object's first)
        void execute(scene::scene& scene, std::size_t index, const budget_decision& decision);

        // After a plan: drops the full mesh data kept for it
        void finish() noexcept;

        // The vertex and index bytes of the current versions
        std::uint64_t used_bytes(const scene::scene& scene) const noexcept;

        // Forgets every entity, without touching the scene (it's about to be destroyed)
        void clear() noexcept
        {
            owners_.clear();
            collected_.clear();
        }

        // The helper drawing the chunk; null when it has none (skipped, or no such chunk)
        scene::entity chunk_helper(scene::entity owner, std::uint32_t chunk) const noexcept;

        // For the budget's tests: the chunk's record
        const dynamic_mesh* chunk_mesh(scene::entity owner, std::uint32_t chunk) const noexcept;

    private:
        struct chunk_state
        {
            std::uint64_t hash = 0;
            bool drawable = true; // false while the chunk's streams don't match: the budget doesn't see it
            dynamic_mesh mesh;
        };

        using owner_state = std::map<std::uint32_t, chunk_state>;

        static void release_all(scene::scene& scene, owner_state& chunks);

        // The chunk's full mesh, its material and its part, then the mesh cut to `vertices`
        result<void> build(
            scene::scene& scene, scene::entity owner, std::uint32_t id, chunk_state& state, std::uint32_t vertices);

        material_cache& materials_;
        std::unordered_map<scene::entity, owner_state> owners_;
        std::vector<std::pair<scene::entity, std::uint32_t>> collected_; // the last collect()'s order
    };
} // namespace avis::visualizer::representation

#endif
