#include "representation/shape_representation.h"

#include "avis/log.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace avis::visualizer::representation
{
    namespace
    {
        using namespace avis::scene::components;
        using scene::entity;

        constexpr const char* logger_name = "visualizer";

        // A frustum's line width, in pixels
        constexpr float frustum_line_width = 1.5f;

        // Lines and translucent boxes don't cast shadows; nothing here is screen-aligned
        constexpr avis::flags<scene::instance_flags> overlay_flags =
            scene::instance_flags::visible | scene::instance_flags::pickable;

        // A zero extent would make a singular world matrix
        Eigen::Vector3f usable_scale(const Eigen::Vector3f& extent) noexcept
        {
            return extent.cwiseMax(Eigen::Vector3f::Constant(1e-3f));
        }

        scene::transform
            scaled(const Eigen::Vector3f& extent, const Eigen::Vector3f& position = Eigen::Vector3f::Zero())
        {
            scene::transform local;
            local.position = position;
            local.scale = usable_scale(extent);
            return local;
        }

        bool same(const scene::transform& a, const scene::transform& b) noexcept
        {
            return a.position == b.position && a.rotation.coeffs() == b.rotation.coeffs() && a.scale == b.scale;
        }
    } // namespace

    void shape_representation::update(scene::scene& scene, const entity owner, const representation_settings& settings)
    {
        if (!scene.alive(owner))
        {
            states_.erase(owner); // its parts were children: they went with it
            return;
        }

        entt::registry& registry = scene.registry();
        const display_shape* shape = registry.try_get<display_shape>(owner);
        const shape_kind kind = shape != nullptr ? shape->shape : shape_kind::none;
        if (kind == shape_kind::none)
        {
            if (const auto found = states_.find(owner); found != states_.end())
            {
                destroy_parts(scene, found->second);
                states_.erase(found);
            }
            return;
        }

        entity_state& state = states_[owner];
        if (state.kind != kind)
        {
            destroy_parts(scene, state);
            state.kind = kind;
        }

        scene::mesh_handle created;
        result<std::vector<part_specification>> specifications = specify(scene, *shape, settings, state, created);
        if (!specifications)
        {
            log::get(logger_name)
                ->error(
                    "representation: the display shape of {:#x} could not be built: {}",
                    registry.get<identity>(owner).id,
                    specifications.error());
            destroy_parts(scene, state);
            states_.erase(owner);
            return;
        }

        // A part that went away (destroyed by someone else) or a different count rebuilds them all
        const bool rebuild = state.parts.size() != specifications->size() ||
                             std::ranges::any_of(state.parts, [&](const entity part) { return !scene.alive(part); });
        if (rebuild)
        {
            destroy_parts(scene, state);
            const identity& who = registry.get<identity>(owner);
            for (const part_specification& specification : *specifications)
            {
                const result<entity> part = scene.create_entity(
                    {
                        .name = std::format("{} · {}", who.name, specification.name),
                        .category = scene::entity_category::helper,
                        .parent = owner,
                        .local = specification.local,
                        .represents = owner,
                    });
                if (!part)
                {
                    log::get(logger_name)->warn("representation: {:#x} can't be drawn: {}", who.id, part.error());
                    destroy_parts(scene, state);
                    break;
                }
                state.parts.push_back(*part);
            }
        }

        for (std::size_t index = 0; index < state.parts.size(); ++index)
        {
            apply(scene, state.parts[index], (*specifications)[index]);
        }
        if (created.valid())
        {
            scene.assets().release(created); // the part holds it now, or nothing does
        }
        if (state.parts.empty())
        {
            states_.erase(owner);
        }
    }

    std::span<const entity> shape_representation::parts(const entity owner) const noexcept
    {
        const auto found = states_.find(owner);
        return found != states_.end() ? std::span<const entity>{ found->second.parts } : std::span<const entity>{};
    }

    result<std::vector<shape_representation::part_specification>> shape_representation::specify(
        scene::scene& scene,
        const display_shape& shape,
        const representation_settings& settings,
        entity_state& state,
        scene::mesh_handle& created)
    {
        const auto material = [&](const material_role role, const float line_width = 1.0f)
        {
            return materials_.get(
                { .role = role, .base_color = shape.shape_color, .opacity = shape.opacity, .line_width = line_width });
        };

        std::vector<part_specification> parts;
        switch (shape.shape)
        {
        case shape_kind::none:
            break;

        case shape_kind::box:
            {
                const result<scene::material_handle> faces = material(material_role::box_faces);
                if (!faces)
                {
                    return std::unexpected(faces.error());
                }
                const result<scene::material_handle> edges =
                    material(material_role::box_edges, settings.boxes.edge_width);
                if (!edges)
                {
                    return std::unexpected(edges.error());
                }
                parts.push_back(
                    { .name = "faces",
                      .mesh = shared_.unit_cube(),
                      .materials = { *faces },
                      .layer = scene::draw_layer::world,
                      .flags = overlay_flags,
                      .local = scaled(shape.extent) });
                parts.push_back(
                    { .name = "edges",
                      .mesh = shared_.cube_edges(),
                      .materials = { *edges },
                      .layer = scene::draw_layer::overlay,
                      .flags = overlay_flags,
                      .local = scaled(shape.extent) });
                break;
            }

        case shape_kind::vehicle:
            {
                const bool ghost = shape.opacity < 1.0f;
                const result<scene::material_handle> body =
                    material(ghost ? material_role::ghost_vehicle : material_role::vehicle);
                if (!body)
                {
                    return std::unexpected(body.error());
                }
                parts.push_back(
                    { .name = "body",
                      .mesh = shared_.vehicle_proxy(),
                      .materials = { *body, ghost ? *body : shared_.wheel_material() },
                      .layer = scene::draw_layer::world,
                      .flags = ghost ? overlay_flags : scene::default_instance_flags,
                      .local = scaled(shape.extent, { vehicle_body_offset * shape.extent.x(), 0.0f, 0.0f }) });
                break;
            }

        case shape_kind::axes:
            parts.push_back(
                { .name = "axes",
                  .mesh = shared_.axes(),
                  .materials = { shared_.axes_material() },
                  .layer = scene::draw_layer::overlay,
                  .flags = overlay_flags,
                  .local = scaled(shape.extent) });
            break;

        case shape_kind::frustum:
            {
                const frustum_parameters wanted{ .horizontal_fov = shape.horizontal_fov,
                                                 .vertical_fov = shape.vertical_fov,
                                                 .range = shape.range };
                scene::mesh_handle mesh;
                const entity existing = state.parts.size() == 1 ? state.parts.front() : entt::null;
                if (existing != entt::null && scene.alive(existing))
                {
                    mesh = scene.registry().get<scene::components::mesh>(existing).asset;
                }
                if (!mesh.valid() || state.frustum != wanted)
                {
                    result<scene::mesh_data> data =
                        meshes::frustum(wanted.horizontal_fov, wanted.vertical_fov, wanted.range);
                    if (!data)
                    {
                        return std::unexpected(std::move(data.error()));
                    }
                    if (mesh.valid())
                    {
                        if (result<void> updated = scene.assets().update_mesh(mesh, std::move(*data)); !updated)
                        {
                            return std::unexpected(std::move(updated.error()));
                        }
                    }
                    else
                    {
                        result<scene::mesh_handle> made = scene.assets().create_mesh(std::move(*data));
                        if (!made)
                        {
                            return std::unexpected(std::move(made.error()));
                        }
                        mesh = *made;
                        created = mesh;
                    }
                    state.frustum = wanted;
                }
                const result<scene::material_handle> lines = material(material_role::lines, frustum_line_width);
                if (!lines)
                {
                    if (created.valid())
                    {
                        scene.assets().release(created);
                        created = {};
                    }
                    return std::unexpected(lines.error());
                }
                parts.push_back(
                    { .name = "frustum",
                      .mesh = mesh,
                      .materials = { *lines },
                      .layer = scene::draw_layer::overlay,
                      .flags = overlay_flags,
                      .local = {} });
                break;
            }
        }
        return parts;
    }

    void shape_representation::destroy_parts(scene::scene& scene, entity_state& state)
    {
        for (const entity part : state.parts)
        {
            if (scene.alive(part))
            {
                static_cast<void>(scene.destroy_entity(part));
            }
        }
        state.parts.clear();
        state.frustum.reset();
    }

    void shape_representation::apply(scene::scene& scene, const entity part, const part_specification& specification)
    {
        entt::registry& registry = scene.registry();

        if (!same(scene.local(part), specification.local))
        {
            scene.set_local(part, specification.local);
        }

        if (const scene::components::mesh* current = registry.try_get<scene::components::mesh>(part);
            current == nullptr || current->asset != specification.mesh)
        {
            registry.emplace_or_replace<scene::components::mesh>(
                part, scene::components::mesh{ .asset = specification.mesh });
        }

        if (const material* current = registry.try_get<material>(part);
            current == nullptr || current->materials != specification.materials)
        {
            registry.emplace_or_replace<material>(part, material{ .materials = specification.materials });
        }

        // The selection flag belongs to the simulation loop, which sets it after the representation
        render_options wanted{ .layer = specification.layer, .flags = specification.flags };
        if (const render_options* current = registry.try_get<render_options>(part))
        {
            if (current->flags.count(scene::instance_flags::selected) != 0)
            {
                wanted.flags |= scene::instance_flags::selected;
            }
            if (current->layer == wanted.layer && current->flags == wanted.flags &&
                current->layer_priority == wanted.layer_priority &&
                current->maximum_view_distance == wanted.maximum_view_distance)
            {
                return;
            }
        }
        registry.emplace_or_replace<render_options>(part, wanted);
    }
} // namespace avis::visualizer::representation
