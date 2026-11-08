#ifndef AVIS_VISUALIZER_REPRESENTATION_GEOMETRY_BUDGET_H
#define AVIS_VISUALIZER_REPRESENTATION_GEOMETRY_BUDGET_H

// The geometry budget (docs/design/12-visualizer.md §9.1): the vertex and index bytes of the current version of every
// dynamic mesh the representation owns (belief geometry chunks, lidar sweeps, later trails; labels and the shared
// meshes are excluded) stay within representation.geometry.budget_mib.
//
// plan_geometry is pure: given every dynamic mesh, what it would cost at full size and what its current version costs,
// it says what to build, keep and release. When everything doesn't fit, it reduces in the order of 12 §9.1:
//   1. drop the oldest lidar sweeps, globally oldest first, down to the newest sweep of each sensor
//   2. truncate progressive meshes to a prefix, largest first (one vertex cap for all), down to 25% of their vertices
//   3. skip new versions, only when 1 and 2 taken all the way still don't fit: a mesh that isn't there yet isn't
//      created, an existing one keeps its previous version. Chunks and trails are skipped first (newest first), then
//      sweeps (oldest first). Steps 1 and 2 then go only as far as the rest needs.
// Each plan starts from the full sizes again, so the smallest reduction that fits is always the one chosen: as room
// returns, skipped meshes come back first, then the prefixes grow, then the sweeps return (the reverse order).
//
// If the budget shrinks below what's already there, previous versions are released too (counted as skipped).
// Executing the plan in its order keeps the bytes within the budget at every step: releases first, then the builds
// that shrink a mesh, then the ones that grow it.

#include <cstdint>
#include <span>
#include <vector>

namespace avis::visualizer::representation
{
    enum class budget_kind : std::uint8_t
    {
        sweep, // a lidar sweep: progressive; step 1 may drop it
        chunk, // belief geometry: progressive when it's points
        trail // a trail block (12.11): never progressive
    };

    // One dynamic mesh as the budget sees it
    struct budget_item
    {
        budget_kind kind = budget_kind::chunk;
        bool progressive = false; // vertices in random order and no indices: any prefix is a uniform subsample

        // At full size. For a progressive mesh the bytes are linear in the vertices.
        std::uint64_t full_bytes = 0;
        std::uint32_t full_vertices = 0;

        // The version in the asset cache; both 0 and `exists` false when there's none
        bool exists = false;
        std::uint64_t current_bytes = 0;
        std::uint32_t current_vertices = 0;

        // The source changed since the current version was built (always true when it doesn't exist)
        bool changed = true;

        // Sweeps: which sensor, and the capture order (older is smaller)
        std::uint64_t sensor = 0;
        std::int64_t age = 0;
    };

    enum class budget_action : std::uint8_t
    {
        keep, // nothing to do: up to date, or skipped (an existing mesh keeps its previous version)
        build, // create or update the mesh with `vertices` vertices (a prefix when fewer than full_vertices)
        release // destroy the mesh (a dropped sweep, or skipped while the budget shrank under it)
    };

    struct budget_decision
    {
        budget_action action = budget_action::keep;
        std::uint32_t vertices = 0; // build: the vertex count to build
        bool dropped = false; // step 1
        bool skipped = false; // step 3: not at its newest version
    };

    // How far the budget reduced the picture (12 §9.1), for the statistics overlay
    struct geometry_reduction
    {
        std::uint32_t sweeps_dropped = 0; // step 1
        float prefix_fraction = 1.0f; // step 2: drawn / full vertices of the progressive meshes drawn
        std::uint32_t skipped = 0; // step 3: meshes not at their newest version, or not there at all
        std::uint64_t used_bytes = 0; // current versions after the plan
        std::uint64_t budget_bytes = 0;

        bool reduced() const noexcept
        {
            return sweeps_dropped != 0 || prefix_fraction < 1.0f || skipped != 0;
        }

        friend bool operator==(const geometry_reduction&, const geometry_reduction&) = default;
    };

    struct geometry_plan
    {
        std::vector<budget_decision> decisions; // one per item, in the items' order
        std::vector<std::uint32_t> order; // the items to act on, in execution order: releases, shrinks, then growth
        geometry_reduction reduction;
    };

    // The smallest share of a progressive mesh step 2 keeps
    inline constexpr float minimum_prefix_fraction = 0.25f;

    // Step 2's vertex cap moves in steps of this many vertices, so a cap under steady pressure doesn't re-version every
    // progressive mesh each tick
    inline constexpr std::uint32_t prefix_quantum = 1024;

    geometry_plan plan_geometry(std::span<const budget_item> items, std::uint64_t budget_bytes);

    // The bytes of a progressive mesh's prefix of `vertices` vertices (full_bytes when it isn't progressive)
    std::uint64_t prefix_bytes(const budget_item& item, std::uint32_t vertices) noexcept;
} // namespace avis::visualizer::representation

#endif
