#include "rendering/draw_lists.h"

#include "avis/detail/check.h"
#include "avis/graphics/linear_arena.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <string_view>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "renderer";

        constexpr std::uint32_t no_bucket = std::numeric_limits<std::uint32_t>::max();

        // Viewport slots of a record: 0 the camera, 1–4 the cascades (the sort key's viewport index)
        constexpr std::size_t viewport_slots = 1 + maximum_shadow_cascades;

        // A record's keys per viewport, and the viewports it is visible in (bit i: viewport index i)
        struct record_keys
        {
            std::array<std::uint64_t, viewport_slots> keys;
            std::uint8_t viewports;
        };

        static_assert(std::is_trivially_copyable_v<record_keys> && std::is_trivially_copyable_v<draw_group>);

        Eigen::Matrix4f matrix_of(const gpu::float4x4& matrix) noexcept
        {
            Eigen::Matrix4f result;
            static_assert(sizeof(gpu::float4x4) == sizeof(float) * 16);
            std::memcpy(result.data(), &matrix, sizeof(gpu::float4x4)); // both column-major
            return result;
        }

        scene::object_id object_id_of(const gpu::instance_data& instance) noexcept
        {
            return (scene::object_id{ instance.object_id_high } << 32) | instance.object_id_low;
        }

        translucency translucency_of(const draw_material& material) noexcept
        {
            if (material.alpha_mode != scene::alpha_mode::blend)
            {
                return translucency::opaque;
            }
            switch (material.blend_operation)
            {
            case scene::blend_operation::additive:
                return translucency::additive;
            case scene::blend_operation::subtractive:
                return translucency::subtractive;
            case scene::blend_operation::normal:
                break;
            }
            return translucency::blended;
        }

        viewport_layer viewport_layer_of(const scene::draw_layer layer) noexcept
        {
            switch (layer)
            {
            case scene::draw_layer::overlay:
                return viewport_layer::overlay;
            case scene::draw_layer::overlay_on_top:
                return viewport_layer::overlay_on_top;
            case scene::draw_layer::world:
                break;
            }
            return viewport_layer::world;
        }

        // The view depth of a world point: its distance in front of the viewpoint, along −Z of the view
        float view_depth(const Eigen::Matrix4f& view_from_world, const Eigen::Vector3f& point) noexcept
        {
            return -(view_from_world.row(2).head<3>().dot(point) + view_from_world(2, 3));
        }

        // The lookups through small direct-mapped caches: a frame has few distinct materials and pipeline sets, and
        // each lookup is a std::function over a hash map. A failed pipeline set is cached too, so it is tried once per
        // build.
        class lookup_cache
        {
        public:
            explicit lookup_cache(const draw_list_lookup& lookup) noexcept : lookup_{ lookup } {}

            draw_material material(const scene::material_handle handle)
            {
                material_entry& entry = materials_[handle.index % cache_size];
                if (!entry.valid || entry.handle != handle)
                {
                    entry = { .handle = handle, .state = lookup_.material(handle), .valid = true };
                }
                return entry.state;
            }

            // nullopt when the lookup failed
            std::optional<std::uint32_t> pipeline_set(const pipeline_set_key& key)
            {
                set_entry& entry = sets_[pipeline_set_key_hash{}(key) % cache_size];
                if (!entry.valid || !(entry.key == key))
                {
                    auto found = lookup_.pipeline_set(key);
                    entry = { .key = key, .set = found ? std::optional{ *found } : std::nullopt, .valid = true };
                }
                return entry.set;
            }

        private:
            static constexpr std::size_t cache_size = 64;

            struct material_entry
            {
                scene::material_handle handle;
                draw_material state;
                bool valid = false;
            };

            struct set_entry
            {
                pipeline_set_key key;
                std::optional<std::uint32_t> set;
                bool valid = false;
            };

            const draw_list_lookup& lookup_;
            std::array<material_entry, cache_size> materials_{};
            std::array<set_entry, cache_size> sets_{};
        };
    } // namespace

    draw_material draw_material_of(const gpu::material_data& data) noexcept
    {
        draw_material material;
        material.shading_model =
            data.shading_model == gpu::shading_model_unlit ? scene::shading_model::unlit : scene::shading_model::lit;
        material.alpha_mode = data.alpha_mode == gpu::alpha_mode_blend ? scene::alpha_mode::blend :
                              data.alpha_mode == gpu::alpha_mode_mask  ? scene::alpha_mode::mask :
                                                                         scene::alpha_mode::opaque;
        material.blend_operation =
            data.blend_operation == gpu::blend_operation_additive    ? scene::blend_operation::additive :
            data.blend_operation == gpu::blend_operation_subtractive ? scene::blend_operation::subtractive :
                                                                       scene::blend_operation::normal;
        material.double_sided = (data.flags & gpu::material_flag_double_sided) != 0;
        return material;
    }

    frustum::frustum(const Eigen::Matrix4f& clip_from_world) noexcept
    {
        const Eigen::Vector4f x = clip_from_world.row(0).transpose();
        const Eigen::Vector4f y = clip_from_world.row(1).transpose();
        const Eigen::Vector4f z = clip_from_world.row(2).transpose();
        const Eigen::Vector4f w = clip_from_world.row(3).transpose();
        const std::array<Eigen::Vector4f, plane_count> planes{ w + x, w - x, w + y, w - y, z, w - z };
        for (std::size_t index = 0; index < plane_count; ++index)
        {
            Eigen::Vector4f plane = planes[index];
            const float length = plane.head<3>().norm();
            // No normal: the far plane of an infinite projection, which every point passes
            plane = length > 1e-12f ? Eigen::Vector4f{ plane / length } : Eigen::Vector4f{ 0.0f, 0.0f, 0.0f, 1.0f };
            x_[index] = plane.x();
            y_[index] = plane.y();
            z_[index] = plane.z();
            w_[index] = plane.w();
            absolute_x_[index] = std::abs(plane.x());
            absolute_y_[index] = std::abs(plane.y());
            absolute_z_[index] = std::abs(plane.z());
        }
    }

    bool frustum::intersects(const scene::aabb& box) const noexcept
    {
        if (box.empty())
        {
            return false;
        }
        return intersects(0.5f * (box.minimum + box.maximum), 0.5f * (box.maximum - box.minimum));
    }

    bool screen_aligned_visible(
        const Eigen::Matrix4f& clip_from_world,
        const Eigen::Vector3f& anchor,
        const scene::aabb& pixel_bounds,
        const float width,
        const float height,
        const float device_pixel_ratio) noexcept
    {
        if (pixel_bounds.empty() || !(width > 0.0f) || !(height > 0.0f))
        {
            return false;
        }
        const Eigen::Vector4f clip = clip_from_world * anchor.homogeneous();
        // In front of the camera, and between the near plane (reverse-Z: depth 1) and the far one (depth 0)
        if (!(clip.w() > 0.0f) || clip.z() > clip.w() || clip.z() < 0.0f)
        {
            return false;
        }
        // The mesh's extent in NDC units, the same on both sides of the anchor
        const Eigen::Vector3f extent = pixel_bounds.minimum.cwiseAbs().cwiseMax(pixel_bounds.maximum.cwiseAbs());
        const float extent_x = 2.0f * extent.x() * device_pixel_ratio / width;
        const float extent_y = 2.0f * extent.y() * device_pixel_ratio / height;
        return std::abs(clip.x()) <= clip.w() * (1.0f + extent_x) && std::abs(clip.y()) <= clip.w() * (1.0f + extent_y);
    }

    void sort_draw_items(const std::span<draw_item> items, graphics::linear_arena& arena)
    {
        // Below this, the passes and their histograms cost more than they save
        constexpr std::size_t radix_threshold = 512;
        if (items.size() < radix_threshold)
        {
            std::ranges::sort(items);
            return;
        }

        // Digits from the least significant: the value's 4 bytes, then the key's 8. Every pass is stable, so the
        // result is ordered by (key, value).
        constexpr std::size_t digit_count = 12;
        const auto digit = [](const draw_item& item, const std::size_t index) noexcept -> std::uint32_t
        {
            return index < 4 ? (item.value >> (8 * index)) & 0xFFu :
                               static_cast<std::uint32_t>(item.key >> (8 * (index - 4))) & 0xFFu;
        };

        // All the histograms in one read
        std::array<std::array<std::uint32_t, 256>, digit_count> counts{};
        for (const draw_item& item : items)
        {
            for (std::size_t index = 0; index < digit_count; ++index)
            {
                ++counts[index][digit(item, index)];
            }
        }

        const std::span<draw_item> scratch = arena.allocate_array<draw_item>(items.size());
        std::span<draw_item> source = items;
        std::span<draw_item> destination = scratch;
        for (std::size_t index = 0; index < digit_count; ++index)
        {
            std::array<std::uint32_t, 256>& column = counts[index];
            // A digit every item shares orders nothing: most of the group bits, the top of the depth field, the high
            // bytes of the value
            if (column[digit(source[0], index)] == items.size())
            {
                continue;
            }
            std::uint32_t offset = 0;
            for (std::uint32_t& count : column)
            {
                const std::uint32_t here = count;
                count = offset;
                offset += here;
            }
            for (const draw_item& item : source)
            {
                destination[column[digit(item, index)]++] = item;
            }
            std::swap(source, destination);
        }
        if (source.data() != items.data())
        {
            std::ranges::copy(source, items.begin());
        }
    }

    std::span<const draw_item> draw_list::range(const sort_key_prefix prefix) const noexcept
    {
        return range(prefix_range(prefix));
    }

    std::span<const draw_item> draw_list::range(const key_range keys) const noexcept
    {
        const auto begin =
            std::ranges::partition_point(items_, [&](const draw_item& item) { return item.key < keys.first; });
        const auto end =
            std::partition_point(begin, items_.end(), [&](const draw_item& item) { return item.key <= keys.last; });
        return { begin, end };
    }

    draw_list
        build_draw_list(const view_description& view, const draw_list_sources& sources, graphics::linear_arena& arena)
    {
        draw_list list;
        draw_list_statistics& statistics = list.statistics_;
        const extraction_result& extraction = sources.extraction;
        const std::span<const gpu::instance_data> instances = extraction.instances;
        const std::span<const draw_mesh> meshes = sources.meshes;
        const std::span<const scene::render_instance> snapshot_instances = sources.snapshot.instances;

        AVIS_CHECK(
            logger_name,
            extraction.instance_sources.size() == instances.size(),
            "extraction_result::instance_sources must have one entry per instance");

        // ---- 1. Records, mesh-major ----
        // Pair (mesh, submesh) has bucket mesh_first[mesh] + submesh
        const std::span<std::uint32_t> mesh_first = arena.allocate_array<std::uint32_t>(meshes.size() + 1);
        mesh_first[0] = 0;
        for (std::size_t index = 0; index < meshes.size(); ++index)
        {
            mesh_first[index + 1] = mesh_first[index] + static_cast<std::uint32_t>(meshes[index].primitives.size());
        }
        const std::uint32_t bucket_count = mesh_first[meshes.size()];

        // The bucket of each instance, or no_bucket when it gets no record
        const std::span<std::uint32_t> bucket_of = arena.allocate_array<std::uint32_t>(instances.size());
        const std::span<std::uint32_t> bucket_first =
            arena.allocate_array<std::uint32_t>(std::size_t{ bucket_count } + 1);
        std::ranges::fill(bucket_first, 0u);
        for (std::size_t index = 0; index < instances.size(); ++index)
        {
            bucket_of[index] = no_bucket;
            const gpu::instance_data& instance = instances[index];
            const std::uint32_t source =
                index < extraction.instance_sources.size() ? extraction.instance_sources[index] : no_bucket;
            if (source >= snapshot_instances.size() || instance.mesh >= meshes.size())
            {
                ++statistics.skipped_invalid;
                continue;
            }
            const scene::render_instance& origin = snapshot_instances[source];
            if (origin.submesh >= meshes[instance.mesh].primitives.size())
            {
                ++statistics.skipped_invalid;
                continue;
            }
            // 11 §3.4: screen-aligned meshes are in pixels, so only the overlay layers can draw them
            if (!AVIS_CHECK(
                    logger_name,
                    !(origin.layer == scene::draw_layer::world &&
                      (origin.flags & scene::instance_flags::screen_aligned)),
                    std::format(
                        "a screen_aligned instance in the world layer isn't drawn (object {})", origin.object_id)))
            {
                ++statistics.skipped_invalid;
                continue;
            }
            bucket_of[index] = mesh_first[instance.mesh] + origin.submesh;
            ++bucket_first[bucket_of[index] + 1];
        }
        for (std::uint32_t bucket = 0; bucket < bucket_count; ++bucket)
        {
            bucket_first[bucket + 1] += bucket_first[bucket];
        }

        const std::uint32_t record_count = bucket_first[bucket_count];
        const std::span<draw_record> records = arena.allocate_array<draw_record>(record_count);
        {
            const std::span<std::uint32_t> next = arena.allocate_array<std::uint32_t>(bucket_count);
            std::copy_n(bucket_first.begin(), bucket_count, next.begin());
            for (std::size_t index = 0; index < instances.size(); ++index)
            {
                if (bucket_of[index] == no_bucket)
                {
                    continue;
                }
                const scene::render_instance& origin = snapshot_instances[extraction.instance_sources[index]];
                records[next[bucket_of[index]]++] = { .instance = static_cast<std::uint32_t>(index),
                                                      .mesh = instances[index].mesh,
                                                      .submesh = origin.submesh,
                                                      .flags = origin.flags };
            }
        }
        // Within a bucket, by object id, so the list doesn't depend on the input order (the instance index breaks the
        // tie of a repeated id)
        const auto by_object = [&](const draw_record& left, const draw_record& right)
        {
            const scene::object_id left_id = object_id_of(instances[left.instance]);
            const scene::object_id right_id = object_id_of(instances[right.instance]);
            return left_id != right_id ? left_id < right_id : left.instance < right.instance;
        };
        for (std::uint32_t bucket = 0; bucket < bucket_count; ++bucket)
        {
            const std::span<draw_record> run =
                records.subspan(bucket_first[bucket], bucket_first[bucket + 1] - bucket_first[bucket]);
            if (run.size() > 1 && !std::ranges::is_sorted(run, by_object))
            {
                std::ranges::sort(run, by_object);
            }
        }
        list.records_ = records;
        statistics.records = record_count;

        // ---- 2. Cull per viewport, and each record's keys ----
        const float width = static_cast<float>(view.size.width);
        const float height = static_cast<float>(view.size.height);
        const Eigen::Matrix4f view_from_world = view.camera.view_from_world();
        const Eigen::Matrix4f camera_clip_from_world =
            view.camera.projection(height > 0.0f ? width / height : 1.0f) * view_from_world;
        const frustum camera_frustum{ camera_clip_from_world };
        const Eigen::Vector3f camera_position = view.camera.world_from_view.translation();

        std::size_t cascade_count = sources.cascades.size();
        if (!AVIS_CHECK(
                logger_name,
                cascade_count <= maximum_shadow_cascades,
                std::format(
                    "{} shadow cascades, the sort key has viewports for {}", cascade_count, maximum_shadow_cascades)))
        {
            cascade_count = maximum_shadow_cascades;
        }
        std::array<std::optional<frustum>, maximum_shadow_cascades> cascade_frusta;
        for (std::size_t cascade = 0; cascade < cascade_count; ++cascade)
        {
            const cascade_viewpoint& viewpoint = sources.cascades[cascade];
            cascade_frusta[cascade].emplace(viewpoint.projection * viewpoint.view_from_world);
        }

        // The record of each instance that has one. The cull walks the instances in order, which reads their arrays
        // sequentially (the records are mesh-major), and writes each record's keys.
        const std::span<std::uint32_t> record_of = arena.allocate_array<std::uint32_t>(instances.size());
        for (std::uint32_t index = 0; index < record_count; ++index)
        {
            record_of[records[index].instance] = index;
        }

        const std::span<record_keys> keys = arena.allocate_array<record_keys>(record_count);
        lookup_cache lookups{ sources.lookup };
        std::size_t item_count = 0;
        for (std::size_t instance_index = 0; instance_index < instances.size(); ++instance_index)
        {
            if (bucket_of[instance_index] == no_bucket)
            {
                continue;
            }
            const draw_record& record = records[record_of[instance_index]];
            record_keys& entry = keys[record_of[instance_index]];
            entry.viewports = 0;

            const gpu::instance_data& instance = instances[instance_index];
            const scene::render_instance& origin = snapshot_instances[extraction.instance_sources[instance_index]];
            const draw_mesh& mesh = meshes[record.mesh];
            const bool screen_aligned = static_cast<bool>(origin.flags & scene::instance_flags::screen_aligned);

            const draw_material material = lookups.material(origin.material);
            const std::optional<std::uint32_t> pipeline_set = lookups.pipeline_set(
                { .shading_model = material.shading_model,
                  .alpha_mode = material.alpha_mode,
                  .blend_operation = material.blend_operation,
                  .topology = mesh.topology,
                  .attributes = mesh.attributes,
                  .double_sided = material.double_sided,
                  .screen_aligned = screen_aligned });
            if (!pipeline_set)
            {
                ++statistics.skipped_no_pipeline;
                continue;
            }

            // World bounds (Arvo's method), or the anchor of a screen-aligned mesh
            const Eigen::Matrix4f world = matrix_of(instance.world);
            const scene::aabb& local_bounds = mesh.primitives[record.submesh].bounds;
            const bool has_bounds = !local_bounds.empty();
            Eigen::Vector3f centre = world.col(3).head<3>();
            Eigen::Vector3f half_size = Eigen::Vector3f::Zero();
            bool in_camera = false;
            if (screen_aligned)
            {
                in_camera = screen_aligned_visible(
                    camera_clip_from_world, centre, local_bounds, width, height, view.device_pixel_ratio);
            }
            else if (has_bounds)
            {
                // Arvo's method, as scene::aabb::transformed: the centre through the world, the half-size through the
                // absolute linear part
                const Eigen::Matrix3f linear = world.topLeftCorner<3, 3>();
                centre += linear * (0.5f * (local_bounds.minimum + local_bounds.maximum));
                half_size = linear.cwiseAbs() * (0.5f * (local_bounds.maximum - local_bounds.minimum));
                in_camera = camera_frustum.intersects(centre, half_size);
            }

            const translucency translucent = translucency_of(material);
            const viewport_layer layer = viewport_layer_of(origin.layer);
            const std::uint32_t material_slot = instance.material;

            if (!in_camera)
            {
                ++statistics.culled_frustum;
            }
            else if (
                origin.maximum_view_distance > 0.0f && (centre - camera_position).norm() > origin.maximum_view_distance)
            {
                ++statistics.culled_distance;
            }
            else
            {
                const std::uint32_t q = quantize_depth(view_depth(view_from_world, centre));
                std::uint32_t depth = 0;
                if (layer == viewport_layer::world)
                {
                    depth = translucent == translucency::opaque ? opaque_depth_field(q, sources.opaque_depth_bands) :
                                                                  translucent_depth_field(q);
                }
                else
                {
                    depth = overlay_depth_field(origin.layer_priority, translucent != translucency::opaque, q);
                }
                entry.keys[0] = make_draw_key(
                    { .group = { .fullscreen_layer = fullscreen_layer::scene,
                                 .viewport = viewport::camera(),
                                 .viewport_layer = layer,
                                 .translucency = translucent },
                      .depth = depth,
                      .pipeline_set = *pipeline_set,
                      .material = material_slot });
                entry.viewports |= 1u;
            }

            // Shadow casters: opaque and masked world geometry that casts shadows, in each cascade it touches
            const bool caster = layer == viewport_layer::world && translucent == translucency::opaque && has_bounds &&
                                static_cast<bool>(origin.flags & scene::instance_flags::casts_shadows);
            if (caster)
            {
                for (std::uint32_t cascade = 0; cascade < cascade_count; ++cascade)
                {
                    if (!cascade_frusta[cascade]->intersects(centre, half_size))
                    {
                        continue;
                    }
                    const cascade_viewpoint& viewpoint = sources.cascades[cascade];
                    const std::uint32_t q = quantize_depth(view_depth(viewpoint.view_from_world, centre));
                    entry.keys[1 + cascade] = make_draw_key(
                        { .group = { .fullscreen_layer = fullscreen_layer::scene,
                                     .viewport = viewport::shadow_cascade(cascade),
                                     .viewport_layer = viewport_layer::world,
                                     .translucency = translucency::opaque },
                          .depth = opaque_depth_field(q, sources.opaque_depth_bands),
                          .pipeline_set = *pipeline_set,
                          .material = material_slot });
                    entry.viewports |= static_cast<std::uint8_t>(1u << (1 + cascade));
                }
            }
            item_count += static_cast<std::size_t>(std::popcount(entry.viewports));
        }

        // ---- 3. Items ----
        std::size_t command_count = 0;
        for (const draw_item& command : sources.commands)
        {
            if (AVIS_CHECK(
                    logger_name,
                    is_command_key(command.key),
                    std::format("a command item without the command bit (key {:#018x}) isn't drawn", command.key)))
            {
                ++command_count;
            }
        }
        const std::span<draw_item> items = arena.allocate_array<draw_item>(item_count + command_count);
        std::size_t written = 0;
        for (std::uint32_t index = 0; index < record_count; ++index)
        {
            const record_keys& entry = keys[index];
            for (std::uint8_t remaining = entry.viewports; remaining != 0; remaining &= remaining - 1)
            {
                const std::size_t slot = static_cast<std::size_t>(std::countr_zero(remaining));
                items[written++] = { .key = entry.keys[slot], .value = index };
                if (slot == 0)
                {
                    ++statistics.camera_items;
                }
                else
                {
                    ++statistics.cascade_items[slot - 1];
                }
            }
        }
        for (const draw_item& command : sources.commands)
        {
            if (is_command_key(command.key))
            {
                items[written++] = command;
            }
        }
        statistics.command_items = command_count;
        statistics.items = items.size();
        if (!sources.command_arguments.empty())
        {
            void* const arguments = arena.allocate(sources.command_arguments.size(), command_argument_alignment);
            std::memcpy(arguments, sources.command_arguments.data(), sources.command_arguments.size());
            list.command_arguments_ = { static_cast<const std::byte*>(arguments), sources.command_arguments.size() };
        }

        // ---- 4. Sort ----
        sort_draw_items(items, arena);
        list.items_ = items;

        // ---- 5. Index the groups: each group's end is a binary search from its start ----
        constexpr std::size_t maximum_groups = std::size_t{ 1 } << 10; // the group is 10 bits
        const std::span<draw_group> groups = arena.allocate_array<draw_group>(std::min(items.size(), maximum_groups));
        std::size_t group_count = 0;
        for (auto first = items.begin(); first != items.end();)
        {
            const key_group group = group_of(first->key);
            const std::uint64_t last_key = prefix_range(key_prefix(group)).last;
            const auto end =
                std::partition_point(first, items.end(), [&](const draw_item& item) { return item.key <= last_key; });
            groups[group_count++] = { .group = group,
                                      .first = static_cast<std::uint32_t>(first - items.begin()),
                                      .count = static_cast<std::uint32_t>(end - first) };
            first = end;
        }
        list.groups_ = groups.first(group_count);
        statistics.groups = group_count;
        return list;
    }
} // namespace avis::visualizer::rendering
