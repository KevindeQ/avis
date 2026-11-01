#ifndef AVIS_SCENE_STATISTICS_H
#define AVIS_SCENE_STATISTICS_H

// Scene statistics (docs/design/08-scene-management.md §8.5): what the scene holds and what its last tick cost. A
// value type, filled by scene::statistics() in O(1) and posted to the GUI at 2 Hz.

#include "avis/scene/types.h"
#include "avis/simulation/time.h"

#include <array>
#include <cstdint>

namespace avis::scene
{
    struct scene_statistics
    {
        simulation::step_index step = 0; // of the last snapshot, as its caller set it before make_snapshot
        std::uint32_t entities = 0;
        std::uint32_t nodes = 0;
        // By entity_category; the last entry, helper, counts the Visualizer's helpers
        std::array<std::uint32_t, entity_category_count> entities_per_category{};
        std::uint32_t instances = 0; // in the last snapshot
        std::uint32_t lights = 0; // in the last snapshot
        std::uint32_t meshes = 0;
        std::uint32_t materials = 0;
        std::uint32_t textures = 0;
        std::uint64_t current_geometry_bytes = 0; // current versions held by the asset cache
        std::uint64_t retained_geometry_bytes = 0; // older versions still held by snapshot slots or uploads (§5.2)
        std::uint32_t labels = 0;
        std::uint32_t trails = 0;
        std::uint32_t trail_samples = 0;
        double transform_update_milliseconds = 0.0; // update_world_transforms and end_tick, the last finished tick
        double snapshot_build_milliseconds = 0.0; // the last make_snapshot
    };
} // namespace avis::scene

#endif
