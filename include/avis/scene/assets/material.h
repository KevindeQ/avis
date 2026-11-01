#ifndef AVIS_SCENE_ASSETS_MATERIAL_H
#define AVIS_SCENE_ASSETS_MATERIAL_H

// Materials: glTF metallic-roughness plus the Avis extensions (docs/design/08-scene-management.md §9.4,
// docs/design/11-renderer.md §3.3)

#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/types.h"

#include <Eigen/Dense>

#include <cstdint>
#include <string>

namespace avis::scene
{
    enum class shading_model : std::uint8_t
    {
        lit,
        unlit
    };

    enum class alpha_mode : std::uint8_t
    {
        opaque,
        mask,
        blend
    };

    // For alpha_mode::blend only
    enum class blend_operation : std::uint8_t
    {
        normal,
        additive,
        subtractive
    };

    struct material_description
    {
        color base_color_factor;
        float metallic_factor = 1.0f;
        float roughness_factor = 1.0f;
        float normal_scale = 1.0f;
        float occlusion_strength = 1.0f;
        Eigen::Vector3f emissive_factor = Eigen::Vector3f::Zero();
        float alpha_cutoff = 0.5f;
        texture_handle base_color_texture; // invalid = none
        texture_handle metallic_roughness_texture;
        texture_handle normal_texture;
        texture_handle occlusion_texture;
        texture_handle emissive_texture;
        shading_model shading = shading_model::lit;
        alpha_mode alpha = alpha_mode::opaque;
        blend_operation blend = blend_operation::normal;
        bool double_sided = false;
        bool use_vertex_color = false;
        float point_size = 0.05f; // metres, point topology
        float line_width = 1.0f; // pixels, line topology
        std::string debug_name;
    };
} // namespace avis::scene

#endif
