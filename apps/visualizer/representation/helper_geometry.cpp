#include "representation/helper_geometry.h"

#include "representation/scene_representation.h"

#include "avis/log.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"

#include <string_view>

namespace avis::visualizer::representation
{
    namespace
    {
        using namespace avis::scene::components;
        using scene::entity;

        constexpr const char* logger_name = "visualizer";
    } // namespace

    void helper_geometry::update(scene::scene& scene, const presentation_state& state)
    {
        ensure(
            scene,
            grid_,
            state.grid,
            "grid",
            shared_.grid(),
            shared_.grid_material(),
            scene::draw_layer::overlay,
            1.0f);
        ensure(
            scene,
            gizmo_,
            state.axes_gizmo,
            "axes gizmo",
            shared_.axes(),
            shared_.axes_material(),
            scene::draw_layer::overlay_on_top,
            axes_gizmo_length);
    }

    entity helper_geometry::find_group(scene::scene& scene)
    {
        if (group_ != entt::null && scene.alive(group_))
        {
            return group_;
        }
        group_ = entt::null;
        for (const auto [candidate, who] : scene.registry().view<identity>().each())
        {
            if (who.category == scene::entity_category::helper && std::string_view{ who.name } == helpers_group_name &&
                !scene.registry().all_of<represents>(candidate))
            {
                group_ = candidate;
                break;
            }
        }
        return group_;
    }

    void helper_geometry::ensure(
        scene::scene& scene,
        entity& slot,
        const bool wanted,
        const char* const name,
        const scene::mesh_handle mesh,
        const scene::material_handle material_handle,
        const scene::draw_layer layer,
        const float scale)
    {
        const bool exists = slot != entt::null && scene.alive(slot);
        if (wanted == exists)
        {
            return;
        }
        if (!wanted)
        {
            static_cast<void>(scene.destroy_entity(slot));
            slot = entt::null;
            return;
        }

        scene::transform local;
        local.scale = Eigen::Vector3f::Constant(scale);
        const result<entity> created = scene.create_entity(
            { .name = name, .category = scene::entity_category::helper, .parent = find_group(scene), .local = local });
        if (!created)
        {
            log::get(logger_name)->error("representation: the {} could not be created: {}", name, created.error());
            slot = entt::null;
            return;
        }
        slot = *created;
        entt::registry& registry = scene.registry();
        registry.emplace<scene::components::mesh>(slot, scene::components::mesh{ .asset = mesh });
        registry.emplace<material>(slot, material{ .materials = { material_handle } });
        registry.emplace<render_options>(
            slot,
            render_options{ .layer = layer,
                            .flags = avis::flags<scene::instance_flags>{ scene::instance_flags::visible } });
    }
} // namespace avis::visualizer::representation
