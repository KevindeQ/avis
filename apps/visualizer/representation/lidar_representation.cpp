#include "representation/lidar_representation.h"

#include "representation/helper_parts.h"

#include "avis/log.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/identity.h"
#include "avis/scene/components/rendering.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <utility>

namespace avis::visualizer::representation
{
    namespace
    {
        using namespace avis::scene::components;
        using scene::entity;

        constexpr const char* logger_name = "visualizer";

        // A turbo-like map without its near-black ends, so small points stay readable on a dark or light ground:
        // blue, light blue, cyan, green, yellow, orange, red
        constexpr std::array<std::array<float, 3>, 7> color_stops{ { { 50.0f, 90.0f, 220.0f },
                                                                     { 40.0f, 170.0f, 245.0f },
                                                                     { 30.0f, 220.0f, 180.0f },
                                                                     { 130.0f, 240.0f, 70.0f },
                                                                     { 240.0f, 220.0f, 50.0f },
                                                                     { 245.0f, 130.0f, 40.0f },
                                                                     { 220.0f, 50.0f, 30.0f } } };

        // Eight colours far apart, for rings
        constexpr std::array<std::array<std::uint8_t, 4>, 8> ring_palette{ { { 230, 25, 75, 255 },
                                                                             { 60, 180, 75, 255 },
                                                                             { 255, 225, 25, 255 },
                                                                             { 0, 130, 200, 255 },
                                                                             { 245, 130, 48, 255 },
                                                                             { 145, 30, 180, 255 },
                                                                             { 70, 240, 240, 255 },
                                                                             { 240, 50, 230, 255 } } };

        bool has_channel(const lidar_sweep& sweep, const lidar_color_mode mode) noexcept
        {
            const std::size_t count = sweep.positions.size();
            switch (mode)
            {
            case lidar_color_mode::intensity:
                return sweep.intensity.size() == count;
            case lidar_color_mode::ring:
                return sweep.ring.size() == count;
            case lidar_color_mode::height:
                return true;
            case lidar_color_mode::uniform:
                return false;
            }
            return false;
        }
    } // namespace

    std::array<std::uint8_t, 4> lidar_color_map(const float t) noexcept
    {
        const float position = std::clamp(std::isnan(t) ? 0.0f : t, 0.0f, 1.0f) * (color_stops.size() - 1);
        const auto low = std::min(static_cast<std::size_t>(position), color_stops.size() - 2);
        const float blend = position - static_cast<float>(low);
        std::array<std::uint8_t, 4> rgba{ 0, 0, 0, 255 };
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const float value =
                color_stops[low][channel] + (color_stops[low + 1][channel] - color_stops[low][channel]) * blend;
            rgba[channel] = static_cast<std::uint8_t>(std::lround(value));
        }
        return rgba;
    }

    result<scene::mesh_data>
        build_sweep_mesh(const lidar_sweep& sweep, const lidar_color_mode mode, const std::uint32_t maximum_points)
    {
        const bool colored = has_channel(sweep, mode);
        const Eigen::Vector3f up =
            sweep.sensor_orientation.conjugate() * Eigen::Vector3f::UnitZ(); // world z, sensor frame
        const auto height = static_cast<float>(sweep.sensor_position.z());

        scene::mesh_builder builder{ scene::mesh_topology::points, true };
        builder.reserve(sweep.positions.size(), 0);
        for (std::size_t index = 0; index < sweep.positions.size(); ++index)
        {
            builder.add_vertex(sweep.positions[index]);
            if (!colored)
            {
                continue;
            }
            switch (mode)
            {
            case lidar_color_mode::intensity:
                builder.color(lidar_color_map(static_cast<float>(sweep.intensity[index]) / 255.0f));
                break;
            case lidar_color_mode::height:
                builder.color(lidar_color_map(
                    (height + up.dot(sweep.positions[index]) - lidar_height_minimum) /
                    (lidar_height_maximum - lidar_height_minimum)));
                break;
            case lidar_color_mode::ring:
                builder.color(ring_palette[sweep.ring[index] % ring_palette.size()]);
                break;
            case lidar_color_mode::uniform:
                break;
            }
        }
        result<scene::mesh_data> data = std::move(builder).build();
        if (data)
        {
            shuffle_progressive(*data);
            truncate_progressive(*data, std::max<std::uint32_t>(maximum_points, 1));
        }
        return data;
    }

    bool lidar_representation::restyle(scene::scene& scene, const lidar_view& view, const lidar_settings& settings)
    {
        const bool rebuild = view.color_mode != view_.color_mode || settings != settings_;
        const bool resize = view.point_size != view_.point_size;
        view_ = view;
        settings_ = settings;
        if (rebuild)
        {
            for (auto& [owner, sweeps] : owners_)
            {
                for (sweep_state& state : sweeps)
                {
                    state.mesh.changed = true;
                    state.mesh.pending.reset();
                    if (const lidar_sweep* sweep = find_sweep(scene, owner, state.capture_time))
                    {
                        if (result<scene::mesh_data> data =
                                build_sweep_mesh(*sweep, view_.color_mode, settings_.maximum_points_per_sweep))
                        {
                            state.mesh.set_pending(std::move(*data));
                        }
                    }
                }
            }
            return !owners_.empty();
        }
        if (resize)
        {
            // Only the material: the meshes stay
            for (auto& [owner, sweeps] : owners_)
            {
                for (sweep_state& state : sweeps)
                {
                    if (!state.mesh.exists(scene))
                    {
                        continue;
                    }
                    const scene::mesh_handle mesh =
                        scene.registry().get<scene::components::mesh>(state.mesh.helper).asset;
                    const scene::versioned_mesh current = scene.assets().current(mesh);
                    const bool colored = current.data && !current.data->color0.empty();
                    if (const result<scene::material_handle> material = material_for(colored))
                    {
                        scene.registry().emplace_or_replace<scene::components::material>(
                            state.mesh.helper, scene::components::material{ .materials = { *material } });
                    }
                }
            }
        }
        return false;
    }

    bool lidar_representation::update(scene::scene& scene, const entity owner)
    {
        const lidar_points* points = scene.alive(owner) ? scene.registry().try_get<lidar_points>(owner) : nullptr;
        const auto found = owners_.find(owner);
        if (points == nullptr)
        {
            if (found == owners_.end())
            {
                return false;
            }
            for (sweep_state& state : found->second)
            {
                release_dynamic_mesh(scene, state.mesh); // under the helpers group: not in the owner's subtree
            }
            owners_.erase(found);
            return true;
        }

        bool changed = false;
        owner_state& sweeps = found != owners_.end() ? found->second : owners_[owner];
        const auto kept = [&](const avis::simulation::simulation_duration time)
        {
            return std::ranges::any_of(
                points->sweeps, [&](const lidar_sweep& sweep) { return sweep.capture_time == time; });
        };

        for (auto state = sweeps.begin(); state != sweeps.end();)
        {
            if (kept(state->capture_time))
            {
                ++state;
                continue;
            }
            release_dynamic_mesh(scene, state->mesh);
            state = sweeps.erase(state);
            changed = true;
        }

        for (const lidar_sweep& sweep : points->sweeps)
        {
            const bool known = std::ranges::any_of(
                sweeps, [&](const sweep_state& state) { return state.capture_time == sweep.capture_time; });
            if (known)
            {
                continue;
            }
            result<scene::mesh_data> data =
                build_sweep_mesh(sweep, view_.color_mode, settings_.maximum_points_per_sweep);
            if (!data)
            {
                log::get(logger_name)
                    ->warn(
                        "representation: a sweep of {:#x} can't be drawn: {}",
                        scene.registry().get<identity>(owner).id,
                        data.error());
                continue;
            }
            // Oldest first, like the component
            const auto position = std::ranges::find_if(
                sweeps, [&](const sweep_state& state) { return state.capture_time > sweep.capture_time; });
            sweep_state& state = *sweeps.insert(position, sweep_state{ .capture_time = sweep.capture_time });
            state.mesh.set_pending(std::move(*data));
            changed = true;
        }
        if (sweeps.empty())
        {
            owners_.erase(owner);
        }
        return changed;
    }

    void lidar_representation::collect(const scene::scene& scene, std::vector<budget_item>& items)
    {
        collected_.clear();
        for (const auto& [owner, sweeps] : owners_)
        {
            for (const sweep_state& state : sweeps)
            {
                items.push_back(state.mesh.item(
                    scene,
                    budget_kind::sweep,
                    static_cast<std::uint64_t>(entt::to_integral(owner)),
                    state.capture_time.count()));
                collected_.emplace_back(owner, state.capture_time);
            }
        }
    }

    void lidar_representation::execute(scene::scene& scene, const std::size_t index, const budget_decision& decision)
    {
        if (index >= collected_.size() || decision.action == budget_action::keep)
        {
            return;
        }
        const auto [owner, time] = collected_[index];
        const auto found = owners_.find(owner);
        if (found == owners_.end())
        {
            return;
        }
        const auto state = std::ranges::find_if(
            found->second, [&](const sweep_state& candidate) { return candidate.capture_time == time; });
        if (state == found->second.end())
        {
            return;
        }
        if (decision.action == budget_action::release)
        {
            release_dynamic_mesh(scene, state->mesh);
            return;
        }
        if (const result<void> built = build(scene, owner, *state, decision.vertices); !built)
        {
            log::get(logger_name)
                ->error(
                    "representation: a sweep of {:#x} could not be built: {}",
                    scene.registry().get<identity>(owner).id,
                    built.error());
        }
    }

    void lidar_representation::finish() noexcept
    {
        for (auto& [owner, sweeps] : owners_)
        {
            for (sweep_state& state : sweeps)
            {
                state.mesh.pending.reset();
            }
        }
    }

    std::uint64_t lidar_representation::used_bytes(const scene::scene& scene) const noexcept
    {
        std::uint64_t bytes = 0;
        for (const auto& [owner, sweeps] : owners_)
        {
            for (const sweep_state& state : sweeps)
            {
                bytes += state.mesh.exists(scene) ? state.mesh.current_bytes : 0;
            }
        }
        return bytes;
    }

    std::size_t lidar_representation::sweep_count(const entity owner) const noexcept
    {
        const auto found = owners_.find(owner);
        return found != owners_.end() ? found->second.size() : 0;
    }

    const dynamic_mesh* lidar_representation::sweep_mesh(const entity owner, const std::size_t index) const noexcept
    {
        const auto found = owners_.find(owner);
        return found != owners_.end() && index < found->second.size() ? &found->second[index].mesh : nullptr;
    }

    const lidar_sweep* lidar_representation::find_sweep(
        const scene::scene& scene, const entity owner, const avis::simulation::simulation_duration time) const
    {
        const lidar_points* points = scene.alive(owner) ? scene.registry().try_get<lidar_points>(owner) : nullptr;
        if (points == nullptr)
        {
            return nullptr;
        }
        const auto found =
            std::ranges::find_if(points->sweeps, [&](const lidar_sweep& sweep) { return sweep.capture_time == time; });
        return found != points->sweeps.end() ? &*found : nullptr;
    }

    result<scene::material_handle> lidar_representation::material_for(const bool colored)
    {
        return materials_.get(
            { .role = material_role::geometry,
              .base_color = colored ? scene::color{} : scene::color::from_srgb8(lidar_uniform_color),
              .point_size = view_.point_size,
              .lit = true,
              .vertex_colors = colored });
    }

    result<void> lidar_representation::build(
        scene::scene& scene, const entity owner, sweep_state& state, const std::uint32_t vertices)
    {
        const lidar_sweep* sweep = find_sweep(scene, owner, state.capture_time);
        if (sweep == nullptr)
        {
            return {}; // gone since update(): the next update releases it
        }
        result<scene::mesh_data> data =
            state.mesh.pending ? std::move(*state.mesh.pending) :
                                 build_sweep_mesh(*sweep, view_.color_mode, settings_.maximum_points_per_sweep);
        state.mesh.pending.reset();
        if (!data)
        {
            return std::unexpected(std::move(data.error()));
        }
        const result<scene::material_handle> material = material_for(!data->color0.empty());
        if (!material)
        {
            return std::unexpected(material.error());
        }

        scene::transform local;
        local.position = sweep->sensor_position.cast<float>();
        local.rotation = sweep->sensor_orientation;
        return build_dynamic_mesh(
            scene,
            state.mesh,
            std::move(*data),
            vertices,
            { .materials = { *material },
              .layer = scene::draw_layer::world,
              .flags = overlay_flags, // point shadows are the renderer's costliest pass (11, as measured)
              .local = local },
            find_helpers_group(scene, group_),
            owner,
            std::format("sweep {:.3f} s", std::chrono::duration<double>(state.capture_time).count()));
    }
} // namespace avis::visualizer::representation
