#ifndef AVIS_SCENE_ASSETS_MESH_VALIDATION_H
#define AVIS_SCENE_ASSETS_MESH_VALIDATION_H

// Private: mesh_data checks and bounds, for the asset cache (docs/design/08-scene-management.md §5.1, §9.4)

#include "avis/error.h"
#include "avis/scene/assets/mesh_data.h"

namespace avis::scene::detail
{
    // Checks the streams and primitives, then computes each primitive's bounds and their union:
    // - every present attribute stream has positions.size() entries, and absent ones are empty
    // - there's at least one primitive, and each lies within the vertex and index streams
    // - each index of an indexed primitive is below its vertex_count (indices are relative to vertex_first)
    // - indices is empty when every primitive has index_count == 0
    // Fails with invalid_mesh naming the first problem found, leaving the data unchanged.
    result<void> validate_and_bound(mesh_data& data);
} // namespace avis::scene::detail

#endif
