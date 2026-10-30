#ifndef AVIS_SCENE_COMPONENTS_MESH_H
#define AVIS_SCENE_COMPONENTS_MESH_H

namespace avis::scene::components
{
    struct mesh
    {
    public:
        // TODO: Add handle to vertex buffer

        int vertex_offset;
        int vertex_count;
    };
} // namespace avis::scene::components

#endif
