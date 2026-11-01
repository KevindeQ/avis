#ifndef AVIS_SCENE_ASSETS_MESH_BUILDER_H
#define AVIS_SCENE_ASSETS_MESH_BUILDER_H

// Builds mesh_data from plain float attributes, encoding normals, tangents and uvs into the Avis vertex format
// (docs/design/08-scene-management.md §9.4, docs/design/11-renderer.md §3.2). For display shapes, belief geometry, lidar
// sweeps, trails and labels.

#include "avis/error.h"
#include "avis/scene/assets/mesh_data.h"

#include <Eigen/Dense>

#include <array>
#include <cstddef>
#include <cstdint>

namespace avis::scene
{
    // ---- vertex encodings ----

    // Octahedral unit vector: the direction projected on the octahedron |x| + |y| + |z| = 1, the lower half folded
    // over the upper, each coordinate in [-1, 1] mapped to the full u16 range. A zero vector encodes as +Z.
    std::array<std::uint16_t, 2> encode_octahedral(const Eigen::Vector3f& direction) noexcept;
    Eigen::Vector3f decode_octahedral(std::array<std::uint16_t, 2> encoded) noexcept; // normalised

    // IEEE 754 binary16 bit patterns, rounded to nearest even; overflow gives infinity
    std::uint16_t encode_f16(float value) noexcept;
    float decode_f16(std::uint16_t bits) noexcept;

    // ---- mesh_builder ----

    class mesh_builder
    {
    public:
        explicit mesh_builder(mesh_topology topology, bool progressive = false);

        // Starts a vertex. The attribute calls below apply to the last added vertex. The first vertex fixes the
        // attribute set: a later vertex with a different one is a debug check, and build() fails.
        mesh_builder& add_vertex(const Eigen::Vector3f& position);
        mesh_builder& normal(const Eigen::Vector3f& normal);
        mesh_builder& tangent(const Eigen::Vector4f& tangent); // xyz direction, w the bitangent sign (glTF)
        mesh_builder& uv(const Eigen::Vector2f& uv);
        mesh_builder& color(std::array<std::uint8_t, 4> srgb);

        // An index relative to the current primitive's first vertex (mesh_primitive)
        mesh_builder& add_index(std::uint32_t index);

        // Closes the primitive started after the previous one: the vertices and indices added since. Does nothing
        // when nothing was added.
        mesh_builder& end_primitive();

        void reserve(std::size_t vertices, std::size_t indices);

        // Closes an open primitive and returns the data, without bounds (the asset cache computes them). A builder
        // with nothing added gives one empty primitive. Fails with invalid_mesh when the vertices don't all have the
        // same attributes.
        result<mesh_data> build() &&;

    private:
        // Compares the last vertex's attributes with the first vertex's, or records them for the first
        void finish_vertex();

        // True when there's a vertex to apply an attribute to (a debug check otherwise)
        bool has_vertex(const char* operation) const noexcept;

        mesh_data data_;
        avis::flags<vertex_attribute> vertex_attributes_{ avis::empty }; // of the vertex being added
        bool attributes_fixed_ = false;
        bool mismatch_ = false;
        std::size_t reserved_vertices_ = 0;
        std::uint32_t primitive_vertex_first_ = 0;
        std::uint32_t primitive_index_first_ = 0;
    };
} // namespace avis::scene

#endif
