#ifndef AVIS_VISUALIZER_REPRESENTATION_MATERIAL_CACHE_H
#define AVIS_VISUALIZER_REPRESENTATION_MATERIAL_CACHE_H

// The representation's materials per role, colour and opacity (docs/design/12-visualizer.md §9), cached by value: 30
// boxes in three colours share three face materials and three edge materials. Owned by the simulation thread.

#include "avis/error.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/material.h"
#include "avis/scene/types.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>

namespace avis::visualizer::representation
{
    enum class material_role : std::uint8_t
    {
        box_faces, // lit, blended with the opacity, double-sided
        box_edges, // unlit lines, opaque, with the edge width
        vehicle, // lit, opaque
        ghost_vehicle, // lit, blended with the opacity (a vehicle whose opacity is below 1)
        lines, // unlit lines, opaque, with the line width (frustums)
        geometry // belief geometry and lidar points: lit or unlit, blended below full opacity, vertex colours or not
    };

    struct material_key
    {
        material_role role = material_role::box_faces;
        scene::color base_color; // linear; its alpha multiplies the opacity
        float opacity = 1.0f;
        float line_width = 1.0f; // pixels, for the line roles and line geometry
        float point_size = 0.05f; // metres, for point geometry
        bool lit = true; // geometry
        bool vertex_colors = false; // geometry: the vertex colours multiply the base colour

        friend bool operator==(const material_key&, const material_key&) = default;
    };

    // The material a key stands for
    scene::material_description describe(const material_key& key);

    class material_cache
    {
    public:
        explicit material_cache(scene::asset_cache& assets) : assets_{ assets } {}
        ~material_cache(); // releases the cache's references; components keep their own

        material_cache(const material_cache&) = delete;
        material_cache& operator=(const material_cache&) = delete;

        // The material for the key, created on first use. The cache keeps one reference; whoever stores the handle in
        // a component gets another from the scene.
        result<scene::material_handle> get(const material_key& key);

        // Forgets every material (after a settings change): entities keep theirs until they're re-represented
        void clear() noexcept;

        std::size_t size() const noexcept
        {
            return materials_.size();
        }

    private:
        using key_tuple = std::tuple<std::uint8_t, float, float, float, float, float, float, bool, bool>;

        static key_tuple to_tuple(const material_key& key) noexcept;

        scene::asset_cache& assets_;
        std::map<key_tuple, scene::material_handle> materials_;
    };
} // namespace avis::visualizer::representation

#endif
