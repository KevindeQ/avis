#include "rendering/progressive_lod.h"

#include "avis/graphics/linear_arena.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::uint32_t no_slot = std::numeric_limits<std::uint32_t>::max();

        // Bisection steps on k: far more than a double's precision needs
        constexpr int bisection_steps = 64;

        Eigen::Matrix4f matrix_of(const gpu::float4x4& matrix) noexcept
        {
            Eigen::Matrix4f result;
            std::memcpy(result.data(), &matrix, sizeof(gpu::float4x4)); // both column-major
            return result;
        }

        // An instance's bounds on n this frame, and what each vertex of n costs
        struct bounded_instance
        {
            double area = 0.0;
            std::uint32_t lower = 0;
            std::uint32_t upper = 0;
            std::uint32_t free_lower = 0; // the bounds without the rate limit
            std::uint32_t free_upper = 0;
            std::uint32_t camera = 0; // 0 or 1
            std::uint32_t shadows = 0;
        };

        // clamp(⌊k · area⌋, lower, upper); an infinite k is the upper bound, an empty area the lower one
        std::uint32_t clamped_count(
            const double k, const double area, const std::uint32_t lower, const std::uint32_t upper) noexcept
        {
            if (std::isinf(k))
            {
                return upper;
            }
            const double wanted = area > 0.0 ? std::floor(k * area) : 0.0;
            return static_cast<std::uint32_t>(
                std::clamp(wanted, static_cast<double>(lower), static_cast<double>(upper)));
        }

        std::uint32_t count_at(const bounded_instance& instance, const double k) noexcept
        {
            return clamped_count(k, instance.area, instance.lower, instance.upper);
        }

        std::uint64_t cost_of(const bounded_instance& instance, const std::uint32_t count, const float scale) noexcept
        {
            return std::uint64_t{ instance.camera } * count +
                   (instance.shadows > 0 ? std::uint64_t{ instance.shadows } * shadow_vertex_count(count, scale) : 0);
        }

        // The view's total at k; monotonic in k
        std::uint64_t total_at(const std::span<const bounded_instance> instances, const double k, const float scale)
        {
            std::uint64_t total = 0;
            for (const bounded_instance& instance : instances)
            {
                total += cost_of(instance, count_at(instance, k), scale);
            }
            return total;
        }
    } // namespace

    std::uint32_t shadow_vertex_count(const std::uint32_t camera_count, const float shadow_lod_scale) noexcept
    {
        const auto scaled =
            static_cast<std::uint32_t>(std::floor(static_cast<double>(shadow_lod_scale) * camera_count));
        return std::max(scaled, 1u);
    }

    float projected_area(
        const Eigen::Vector3f& centre,
        const float radius,
        const camera& camera,
        const graphics::texture_extent size) noexcept
    {
        const float width = static_cast<float>(size.width);
        const float height = static_cast<float>(size.height);
        const float view_area = width * height;
        if (!(radius > 0.0f))
        {
            return 0.0f;
        }
        const float distance = (centre - camera.world_from_view.translation()).norm();
        if (distance <= radius)
        {
            return view_area;
        }
        // The cone tangent to the sphere has half-angle asin(r / d); its radius on the image plane is f · tan of that
        const float focal = 0.5f * height / std::tan(0.5f * camera.vertical_field_of_view);
        const float projected_radius = focal * radius / std::sqrt(distance * distance - radius * radius);
        return std::min(std::numbers::pi_v<float> * projected_radius * projected_radius, view_area);
    }

    progressive_lod_statistics progressive_lod::update(
        const std::span<const progressive_instance> instances,
        const progressive_lod_parameters& parameters,
        const std::span<progressive_vertex_counts> counts)
    {
        progressive_lod_statistics statistics{ .instances = instances.size(), .budget = parameters.vertex_budget };
        const float scale = parameters.shadow_lod_scale;
        const double change = std::clamp(static_cast<double>(parameters.maximum_change), 0.0, 1.0);

        std::vector<bounded_instance> bounded(instances.size());
        for (std::size_t index = 0; index < instances.size(); ++index)
        {
            const progressive_instance& instance = instances[index];
            bounded_instance& entry = bounded[index];
            const std::uint32_t whole = instance.vertex_count;
            entry.area = std::max(static_cast<double>(instance.projected_area), 0.0);
            entry.camera = instance.camera ? 1 : 0;
            entry.shadows = instance.shadow_viewports;
            entry.free_lower = std::min(std::max(parameters.minimum_vertices, 1u), whole);
            entry.free_upper = whole;
            entry.lower = entry.free_lower;
            entry.upper = entry.free_upper;
            statistics.full_vertices += cost_of(entry, whole, scale);

            const auto found = instance.history_invalid ? previous_.end() : previous_.find(instance.object_id);
            if (found != previous_.end() && whole > 0)
            {
                // At most ±maximum_change of the previous n, and at least one vertex of growth
                const double previous = found->second;
                const auto lowest = static_cast<std::uint32_t>(std::ceil(previous * (1.0 - change)));
                const auto highest =
                    std::max(static_cast<std::uint32_t>(std::floor(previous * (1.0 + change))), found->second + 1);
                entry.lower = std::min(std::max(entry.lower, lowest), whole);
                entry.upper = std::max(std::min(entry.upper, highest), entry.lower);
            }
        }

        // The k from which every instance is at its upper bound: a power of two, so that bisecting from it hits round
        // values of k exactly
        double maximum_k = 1.0;
        for (const bounded_instance& entry : bounded)
        {
            while (entry.area > 0.0 && maximum_k * entry.area < entry.upper)
            {
                maximum_k *= 2.0;
            }
        }

        // The largest k whose total fits; the lower bounds are scaled down when they alone don't fit
        double k = std::numeric_limits<double>::infinity();
        if (total_at(bounded, k, scale) > parameters.vertex_budget)
        {
            std::uint64_t lowest_total = total_at(bounded, 0.0, scale);
            if (lowest_total > parameters.vertex_budget)
            {
                statistics.minimums_scaled = true;
                const double factor = static_cast<double>(parameters.vertex_budget) / static_cast<double>(lowest_total);
                for (bounded_instance& entry : bounded)
                {
                    if (entry.lower > 0)
                    {
                        entry.lower = std::max(static_cast<std::uint32_t>(std::floor(entry.lower * factor)), 1u);
                        entry.upper = std::max(entry.upper, entry.lower);
                    }
                }
                lowest_total = total_at(bounded, 0.0, scale);
            }
            if (lowest_total > parameters.vertex_budget)
            {
                k = 0.0; // more instances than the budget has vertices: one each
            }
            else
            {
                double fits = 0.0;
                double too_many = maximum_k;
                for (int step = 0; step < bisection_steps; ++step)
                {
                    const double middle = 0.5 * (fits + too_many);
                    (total_at(bounded, middle, scale) <= parameters.vertex_budget ? fits : too_many) = middle;
                }
                k = fits;
            }
        }
        statistics.vertices_per_pixel = k;

        next_.clear();
        for (std::size_t index = 0; index < instances.size(); ++index)
        {
            const bounded_instance& entry = bounded[index];
            const std::uint32_t count = count_at(entry, k);
            const std::uint32_t free_count = clamped_count(k, entry.area, entry.free_lower, entry.free_upper);
            if (count != free_count && !statistics.minimums_scaled)
            {
                ++statistics.rate_limited;
            }
            if (instances[index].vertex_count == 0)
            {
                counts[index] = {};
                continue;
            }
            counts[index] = { .camera = count, .shadow = shadow_vertex_count(count, scale) };
            statistics.camera_vertices += std::uint64_t{ entry.camera } * count;
            statistics.shadow_vertices += std::uint64_t{ entry.shadows } * counts[index].shadow;
            next_[instances[index].object_id] = count;
        }
        std::swap(previous_, next_);
        return statistics;
    }

    progressive_draw_counts compute_progressive_lod(
        const view_description& view,
        const draw_list& list,
        const std::span<const draw_mesh> meshes,
        const std::span<const gpu::instance_data> instances,
        const progressive_lod_parameters& parameters,
        progressive_lod& lod,
        graphics::linear_arena& arena)
    {
        const std::span<const draw_record> records = list.records();
        const std::span<progressive_vertex_counts> by_record =
            arena.allocate_array<progressive_vertex_counts>(records.size());
        std::ranges::fill(by_record, progressive_vertex_counts{});

        // The progressive records with items, in item order, each once
        const std::span<std::uint32_t> slot_of = arena.allocate_array<std::uint32_t>(records.size());
        std::ranges::fill(slot_of, no_slot);
        std::vector<progressive_instance> gathered;
        std::vector<std::uint32_t> record_of;
        for (const draw_item& item : list.items())
        {
            if (is_command_key(item.key) || item.value >= records.size())
            {
                continue;
            }
            const draw_record& record = records[item.value];
            if (record.mesh >= meshes.size() || record.instance >= instances.size())
            {
                continue;
            }
            const draw_mesh& mesh = meshes[record.mesh];
            if (!mesh.progressive || record.submesh >= mesh.primitives.size())
            {
                continue;
            }
            const scene::mesh_primitive& primitive = mesh.primitives[record.submesh];
            if (primitive.index_count > 0 && mesh.topology != scene::mesh_topology::points)
            {
                continue; // drawn whole
            }

            std::uint32_t& slot = slot_of[item.value];
            if (slot == no_slot)
            {
                slot = static_cast<std::uint32_t>(gathered.size());
                const gpu::instance_data& instance = instances[record.instance];
                float area = 0.0f;
                if (!primitive.bounds.empty())
                {
                    // The sphere around the world bounds (Arvo's method, as the draw list culls)
                    const Eigen::Matrix4f world = matrix_of(instance.world);
                    const Eigen::Matrix3f linear = world.topLeftCorner<3, 3>();
                    const Eigen::Vector3f centre =
                        world.col(3).head<3>() +
                        linear * (0.5f * (primitive.bounds.minimum + primitive.bounds.maximum));
                    const Eigen::Vector3f half_size =
                        linear.cwiseAbs() * (0.5f * (primitive.bounds.maximum - primitive.bounds.minimum));
                    area = projected_area(centre, half_size.norm(), view.camera, view.size);
                }
                gathered.push_back(
                    { .object_id = (scene::object_id{ instance.object_id_high } << 32) | instance.object_id_low,
                      .vertex_count = primitive.vertex_count,
                      .projected_area = area,
                      .history_invalid = static_cast<bool>(record.flags & scene::instance_flags::history_invalid) });
                record_of.push_back(item.value);
            }
            progressive_instance& entry = gathered[slot];
            if (key_fields::viewport.extract(item.key) == 0)
            {
                entry.camera = true;
            }
            else
            {
                ++entry.shadow_viewports;
            }
        }

        std::vector<progressive_vertex_counts> counts(gathered.size());
        progressive_draw_counts result{ .by_record = by_record,
                                        .statistics = lod.update(gathered, parameters, counts) };
        for (std::size_t index = 0; index < gathered.size(); ++index)
        {
            by_record[record_of[index]] = counts[index];
        }
        return result;
    }
} // namespace avis::visualizer::rendering
