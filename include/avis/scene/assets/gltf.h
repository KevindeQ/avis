#ifndef AVIS_SCENE_ASSETS_GLTF_H
#define AVIS_SCENE_ASSETS_GLTF_H

// What a glTF load gives back (docs/design/08-scene-management.md §5.1, §9.4). The loading sender comes with the glTF
// import (docs/tasks/10-scene-management.md §10.6).

#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace avis::scene
{
    struct gltf_node
    {
        std::string name;
        transform local; // already in Avis axes (Z up)
        std::int32_t parent = -1; // index into gltf_asset_set::nodes
        mesh_handle mesh; // invalid = no mesh
        std::vector<material_handle> materials; // one per primitive
    };

    // Holds one reference to each handle; release with asset_cache::release_all
    struct gltf_asset_set
    {
        std::filesystem::path path;
        std::vector<mesh_handle> meshes;
        std::vector<material_handle> materials;
        std::vector<texture_handle> textures;
        std::vector<gltf_node> nodes; // parents before children
    };
} // namespace avis::scene

#endif
