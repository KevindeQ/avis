#include "representation/material_cache.h"

#include <utility>

namespace avis::visualizer::representation
{
    scene::material_description describe(const material_key& key)
    {
        scene::material_description description;
        description.base_color_factor = key.base_color;
        description.metallic_factor = 0.0f;
        description.line_width = key.line_width;

        const float alpha = key.base_color.alpha * key.opacity;
        switch (key.role)
        {
        case material_role::box_faces:
            description.base_color_factor.alpha = alpha;
            description.roughness_factor = 0.6f;
            description.alpha = scene::alpha_mode::blend;
            description.double_sided = true; // the far faces show through the near ones
            description.debug_name = "box faces";
            break;
        case material_role::box_edges:
            description.base_color_factor.alpha = 1.0f;
            description.shading = scene::shading_model::unlit;
            description.debug_name = "box edges";
            break;
        case material_role::vehicle:
            description.base_color_factor.alpha = 1.0f;
            description.roughness_factor = 0.45f;
            description.debug_name = "vehicle";
            break;
        case material_role::ghost_vehicle:
            description.base_color_factor.alpha = alpha;
            description.roughness_factor = 0.45f;
            description.alpha = scene::alpha_mode::blend;
            description.debug_name = "ghost vehicle";
            break;
        case material_role::lines:
            description.base_color_factor.alpha = 1.0f;
            description.shading = scene::shading_model::unlit;
            description.debug_name = "lines";
            break;
        case material_role::geometry:
            description.base_color_factor.alpha = alpha;
            description.roughness_factor = 0.8f;
            description.shading = key.lit ? scene::shading_model::lit : scene::shading_model::unlit;
            description.alpha = alpha < 1.0f ? scene::alpha_mode::blend : scene::alpha_mode::opaque;
            description.double_sided = true; // surfaces from the Driver have no agreed winding
            description.use_vertex_color = key.vertex_colors;
            description.point_size = key.point_size;
            description.debug_name = "geometry";
            break;
        }
        return description;
    }

    material_cache::~material_cache()
    {
        clear();
    }

    result<scene::material_handle> material_cache::get(const material_key& key)
    {
        const key_tuple index = to_tuple(key);
        if (const auto found = materials_.find(index); found != materials_.end())
        {
            return found->second;
        }
        result<scene::material_handle> created = assets_.create_material(describe(key));
        if (!created)
        {
            return std::unexpected(std::move(created.error()));
        }
        materials_.emplace(index, *created);
        return *created;
    }

    void material_cache::clear() noexcept
    {
        for (const auto& [key, material] : materials_)
        {
            assets_.release(material);
        }
        materials_.clear();
    }

    material_cache::key_tuple material_cache::to_tuple(const material_key& key) noexcept
    {
        // Only what describe() reads: the line width doesn't separate face materials
        const bool geometry = key.role == material_role::geometry;
        const bool lines = key.role == material_role::box_edges || key.role == material_role::lines || geometry;
        const bool blended =
            key.role == material_role::box_faces || key.role == material_role::ghost_vehicle || geometry;
        return { static_cast<std::uint8_t>(key.role),
                 key.base_color.red,
                 key.base_color.green,
                 key.base_color.blue,
                 blended ? key.base_color.alpha * key.opacity : 1.0f,
                 lines ? key.line_width : 0.0f,
                 geometry ? key.point_size : 0.0f,
                 geometry && key.lit,
                 geometry && key.vertex_colors };
    }
} // namespace avis::visualizer::representation
