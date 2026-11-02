#include "rendering/extraction.h"

#include "avis/detail/check.h"
#include "avis/log.h"
#include "avis/scene/flags.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "renderer";
        constexpr auto warning_interval = std::chrono::seconds{ 1 };

        static_assert(sizeof(Eigen::Matrix4f) == sizeof(gpu::float4x4), "a Matrix4f copies straight into a float4x4");

        // instance_flags and the shaders' instance_flag_* are the same bits
        static_assert(static_cast<std::uint32_t>(scene::instance_flags::visible) == gpu::instance_flag_visible);
        static_assert(
            static_cast<std::uint32_t>(scene::instance_flags::casts_shadows) == gpu::instance_flag_casts_shadows);
        static_assert(static_cast<std::uint32_t>(scene::instance_flags::selected) == gpu::instance_flag_selected);
        static_assert(static_cast<std::uint32_t>(scene::instance_flags::pickable) == gpu::instance_flag_pickable);
        static_assert(
            static_cast<std::uint32_t>(scene::instance_flags::screen_aligned) == gpu::instance_flag_screen_aligned);
        static_assert(
            static_cast<std::uint32_t>(scene::instance_flags::history_invalid) == gpu::instance_flag_history_invalid);

        gpu::float4x4 to_gpu(const Eigen::Matrix4f& matrix) noexcept
        {
            gpu::float4x4 converted;
            std::memcpy(&converted, matrix.data(), sizeof(converted)); // both column-major
            return converted;
        }

        gpu::float3 to_gpu(const Eigen::Vector3f& vector) noexcept
        {
            return { vector.x(), vector.y(), vector.z() };
        }

        // The inverse transpose of the upper 3 × 3, in a float4x4 (identity elsewhere). A singular matrix (a zero
        // scale) has no normal matrix: the identity keeps the shaders' normalisation finite.
        Eigen::Matrix4f normal_matrix(const Eigen::Matrix4f& world) noexcept
        {
            Eigen::Matrix4f normal = Eigen::Matrix4f::Identity();
            const Eigen::Matrix3f linear = world.topLeftCorner<3, 3>();
            Eigen::Matrix3f inverse;
            bool invertible = false;
            linear.computeInverseWithCheck(inverse, invertible, 1e-30f);
            if (invertible)
            {
                normal.topLeftCorner<3, 3>() = inverse.transpose();
            }
            return normal;
        }

        std::uint64_t mesh_key(const scene::mesh_handle handle) noexcept
        {
            return (std::uint64_t{ handle.index } << 32) | handle.generation;
        }

        gpu::light_data to_light_data(const scene::light_instance& light)
        {
            const Eigen::Vector3f position = light.world.col(3).head<3>();
            Eigen::Vector3f direction = -light.world.col(2).head<3>(); // −Z is the light direction
            if (direction.squaredNorm() > 0.0f)
            {
                direction.normalize();
            }
            else
            {
                direction = -Eigen::Vector3f::UnitZ();
            }

            std::uint32_t kind = gpu::light_kind_directional;
            switch (light.kind)
            {
            case scene::light_kind::directional:
                kind = gpu::light_kind_directional;
                break;
            case scene::light_kind::point:
                kind = gpu::light_kind_point;
                break;
            case scene::light_kind::spot:
                kind = gpu::light_kind_spot;
                break;
            }

            return {
                .position = to_gpu(position),
                .range = light.range,
                .direction = to_gpu(direction),
                .kind = kind,
                .color = { light.light_color.red, light.light_color.green, light.light_color.blue },
                .intensity = light.intensity,
                .spot_cos_inner = std::cos(light.inner_cone_angle),
                .spot_cos_outer = std::cos(light.outer_cone_angle),
                .casts_shadows = 0, // the sun's is set by the caller: only the sun casts shadows (11 §6.2)
                .padding0 = 0,
            };
        }
    } // namespace

    frame_interpolation
        interpolate(const scene::snapshot& snapshot, const std::chrono::steady_clock::time_point wall_now)
    {
        const simulation::time_anchor& anchor = snapshot.anchor;
        const simulation::simulation_duration previous = snapshot.time_previous;
        const simulation::simulation_duration current = snapshot.time_current;
        if (anchor.rate == 0.0 || current <= previous)
        {
            return { .t_render = current, .alpha = 1.0f };
        }

        const double elapsed =
            std::chrono::duration<double, std::nano>(wall_now - anchor.wall_base).count() * anchor.rate;
        const double unclamped = static_cast<double>(anchor.t_base.count()) + elapsed;
        const double clamped =
            std::clamp(unclamped, static_cast<double>(previous.count()), static_cast<double>(current.count()));
        const auto t_render = simulation::simulation_duration{ static_cast<std::int64_t>(std::llround(clamped)) };

        const double alpha =
            static_cast<double>((t_render - previous).count()) / static_cast<double>((current - previous).count());
        return { .t_render = t_render, .alpha = std::clamp(static_cast<float>(alpha), 0.0f, 1.0f) };
    }

    Eigen::Matrix4f
        interpolate_world(const Eigen::Matrix4f& previous, const Eigen::Matrix4f& current, const float alpha)
    {
        if (alpha >= 1.0f || previous == current)
        {
            return current;
        }
        if (alpha <= 0.0f)
        {
            return previous;
        }

        const scene::transform from = scene::transform::from_affine(Eigen::Affine3f{ previous });
        const scene::transform to = scene::transform::from_affine(Eigen::Affine3f{ current });
        const scene::transform between{
            .position = from.position + alpha * (to.position - from.position),
            .rotation = from.rotation.slerp(alpha, to.rotation), // Eigen's slerp takes the shortest arc
            .scale = from.scale + alpha * (to.scale - from.scale),
        };
        return between.to_affine().matrix();
    }

    void extraction_result::clear() noexcept
    {
        interpolation = {};
        publish_sequence = 0;
        instances.clear();
        instance_sources.clear();
        meshes.clear();
        lights.clear();
        sun.reset();
        stationary = true;
        skipped_invisible = 0;
        skipped_not_resident = 0;
        skipped_invalid = 0;
        dropped_lights = 0;
    }

    const Eigen::Matrix4f* extraction_state::previous_world(const scene::object_id id) const
    {
        const auto found = previous_worlds_.find(id);
        return found != previous_worlds_.end() ? &found->second : nullptr;
    }

    void extraction_state::clear() noexcept
    {
        previous_worlds_.clear();
        next_worlds_.clear();
    }

    void extract(
        const scene::snapshot& snapshot,
        const std::chrono::steady_clock::time_point wall_now,
        extraction_state& state,
        const extraction_lookup& lookup,
        extraction_result& result)
    {
        result.clear();
        result.interpolation = interpolate(snapshot, wall_now);
        result.publish_sequence = snapshot.publish_sequence;
        const float alpha = result.interpolation.alpha;

        // Steps 2 and 3: instances, with the previous rendered world of their object
        std::unordered_map<scene::object_id, Eigen::Matrix4f>& next_worlds = state.next_worlds_;
        next_worlds.clear();
        std::unordered_map<std::uint64_t, std::uint32_t> mesh_indices; // mesh_key → index in result.meshes
        result.instances.reserve(snapshot.instances.size());
        result.instance_sources.reserve(snapshot.instances.size());

        for (std::size_t source = 0; source < snapshot.instances.size(); ++source)
        {
            const scene::render_instance& instance = snapshot.instances[source];
            if (!(instance.flags & scene::instance_flags::visible))
            {
                ++result.skipped_invisible;
                continue;
            }
            if (!AVIS_CHECK(
                    logger_name,
                    !(instance.layer == scene::draw_layer::world &&
                      (instance.flags & scene::instance_flags::screen_aligned)),
                    "extraction: a screen_aligned instance in the world layer; screen-aligned instances are only "
                    "valid in the overlay layers (11 §3.4)"))
            {
                ++result.skipped_invalid;
                continue;
            }

            const std::optional<std::uint64_t> version = lookup.mesh_version(instance.mesh);
            if (!version)
            {
                ++result.skipped_not_resident; // 11 §3.5 step 2: drawn once a version is resident
                continue;
            }
            const auto [mesh_index, added] =
                mesh_indices.try_emplace(mesh_key(instance.mesh), static_cast<std::uint32_t>(result.meshes.size()));
            if (added)
            {
                result.meshes.push_back({ .handle = instance.mesh, .version = *version });
            }

            const bool history_invalid = static_cast<bool>(instance.flags & scene::instance_flags::history_invalid);
            const Eigen::Matrix4f world = history_invalid ?
                                              instance.world_current :
                                              interpolate_world(instance.world_previous, instance.world_current, alpha);

            // A pop-in or a jump starts without motion; so does an object the previous frame didn't draw. Every
            // instance of an object (one per submesh) shares its world.
            next_worlds.try_emplace(instance.object_id, world);
            const Eigen::Matrix4f* previous = state.previous_world(instance.object_id);
            const bool restart = history_invalid || instance.world_previous == instance.world_current;
            const Eigen::Matrix4f& world_previous = (previous == nullptr || restart) ? world : *previous;
            if (world_previous != world)
            {
                result.stationary = false;
            }

            result.instances.push_back(
                {
                    .world = to_gpu(world),
                    .world_previous = to_gpu(world_previous),
                    .normal_from_object = to_gpu(normal_matrix(world)),
                    .mesh = mesh_index->second,
                    .material = lookup.material_slot(instance.material),
                    .flags = static_cast<std::uint32_t>(instance.flags.underlying_value()),
                    .layer_priority = instance.layer_priority,
                    .object_id_low = static_cast<std::uint32_t>(instance.object_id),
                    .object_id_high = static_cast<std::uint32_t>(instance.object_id >> 32),
                    .padding0 = 0,
                    .padding1 = 0,
                });
            result.instance_sources.push_back(static_cast<std::uint32_t>(source));
        }

        // Objects not drawn this frame are forgotten
        std::swap(state.previous_worlds_, next_worlds);

        // Step 4: lights
        const std::size_t light_count = std::min(snapshot.lights.size(), maximum_lights);
        result.dropped_lights = snapshot.lights.size() - light_count;
        result.lights.reserve(light_count);
        for (std::size_t index = 0; index < light_count; ++index)
        {
            const scene::light_instance& light = snapshot.lights[index];
            gpu::light_data data = to_light_data(light);
            if (light.kind == scene::light_kind::directional && !result.sun)
            {
                result.sun = static_cast<std::uint32_t>(result.lights.size());
                data.casts_shadows = light.casts_shadows ? 1u : 0u;
            }
            result.lights.push_back(data);
        }
        if (result.dropped_lights > 0)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!state.last_light_warning_ || now - *state.last_light_warning_ >= warning_interval)
            {
                state.last_light_warning_ = now;
                log::get(logger_name)
                    ->warn(
                        "extraction: the snapshot has {} lights, {} are drawn (11 §6.2); the last {} are dropped",
                        snapshot.lights.size(),
                        maximum_lights,
                        result.dropped_lights);
            }
        }
    }

    extraction_result extract(
        const scene::snapshot& snapshot,
        const std::chrono::steady_clock::time_point wall_now,
        extraction_state& state,
        const extraction_lookup& lookup)
    {
        extraction_result result;
        extract(snapshot, wall_now, state, lookup, result);
        return result;
    }
} // namespace avis::visualizer::rendering
