#ifndef AVIS_SCENE_ASSETS_MESH_DATA_H
#define AVIS_SCENE_ASSETS_MESH_DATA_H

// Asset handles and CPU geometry in the Avis vertex format (docs/design/08-scene-management.md §5.1, §9.4;
// docs/design/11-renderer.md §3.2)

#include "avis/enum_flags.h"
#include "avis/scene/bounds.h"

#include <Eigen/Dense>

#include <array>
#include <compare>
#include <cstdint>
#include <vector>

namespace avis::scene
{
    struct mesh_asset;
    struct material_asset;
    struct texture_asset;

    // A reference into the asset cache's slot table for T. Generational: a handle to a freed slot is stale.
    template<typename T>
    struct asset_handle
    {
        std::uint32_t index = 0;
        std::uint32_t generation = 0; // 0 = invalid

        bool valid() const noexcept
        {
            return generation != 0;
        }

        friend auto operator<=>(const asset_handle&, const asset_handle&) = default;
    };

    using mesh_handle = asset_handle<mesh_asset>;
    using material_handle = asset_handle<material_asset>;
    using texture_handle = asset_handle<texture_asset>;

    enum class mesh_topology : std::uint8_t
    {
        triangles,
        lines,
        points
    };

    // The optional vertex streams; positions are always present
    enum class vertex_attribute : std::uint8_t
    {
        normal = 1 << 0,
        tangent = 1 << 1,
        uv0 = 1 << 2,
        color0 = 1 << 3
    };
} // namespace avis::scene

ALLOW_FLAGS_FOR_ENUM(avis::scene::vertex_attribute)

namespace avis::scene
{
    // A draw range of a mesh. Its vertices are [vertex_first, vertex_first + vertex_count). An indexed primitive
    // draws indices [index_first, index_first + index_count), each relative to vertex_first (the draw's base
    // vertex), so every index is below vertex_count. A non-indexed primitive (index_count == 0) draws its vertices in
    // order.
    struct mesh_primitive
    {
        std::uint32_t index_first = 0;
        std::uint32_t index_count = 0; // 0 for a non-indexed primitive
        std::uint32_t vertex_first = 0;
        std::uint32_t vertex_count = 0;
        aabb bounds; // local, of the primitive's vertex range; computed by the cache (§5.1)
    };

    struct mesh_data
    {
        mesh_topology topology = mesh_topology::triangles;
        bool progressive = false; // vertices in random order (11 §9.3)
        avis::flags<vertex_attribute> attributes{ avis::empty };
        std::vector<Eigen::Vector3f> positions; // always present
        std::vector<std::array<std::uint16_t, 2>> normals; // octahedral, when attributes has normal
        std::vector<std::array<std::uint16_t, 2>> tangents; // octahedral; the sign is in tangent_signs
        std::vector<std::int8_t> tangent_signs; // +1 or -1, when attributes has tangent
        std::vector<std::array<std::uint16_t, 2>> uv0; // f16x2 bit patterns
        std::vector<std::array<std::uint8_t, 4>> color0; // sRGB u8x4
        std::vector<std::uint32_t> indices;
        std::vector<mesh_primitive> primitives; // at least one
        aabb bounds; // union of the primitives' bounds

        // Vertex and index bytes of the encoded streams, for budgets and statistics
        std::uint64_t byte_size() const noexcept;
    };
} // namespace avis::scene

#endif
