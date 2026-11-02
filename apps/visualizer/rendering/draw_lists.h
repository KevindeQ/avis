#ifndef AVIS_VISUALIZER_RENDERING_DRAW_LISTS_H
#define AVIS_VISUALIZER_RENDERING_DRAW_LISTS_H

// Draw lists (docs/design/11-renderer.md §4.1, §4.4–§4.6, docs/tasks/11-renderer.md §11.8): per view, the frame's
// visible instances as (key, value) items sorted on their 64-bit key (sort_key.h). build_draw_list runs single-threaded
// on the frame chain's prepare stage, in five steps:
//   1. Records: one draw_record per extracted instance, laid out mesh-major. The instances are counted per
//      (mesh, submesh), a prefix sum gives each pair its first record, and the records are scattered; within a pair
//      they are in object id order, so the list doesn't depend on the order of the input.
//   2. Cull per viewport: the camera frustum against the world bounds (the submesh's bounds through the interpolated
//      world, Arvo's method) and maximum_view_distance; each shadow cascade's frustum for the shadow casters (world
//      layer, opaque or mask material, casts_shadows). A screen_aligned instance is tested by its anchor against the
//      frustum widened by the mesh's pixel bounds.
//   3. Items: one per (record, viewport it is visible in), keyed by 11 §4.2's mapping table, plus the caller's command
//      items, in one arena array sized from the cull counts.
//   4. Sort on (key, value), a total order: std::ranges::sort for short lists, an LSD radix sort for long ones
//      (sort_draw_items). Both give the same order.
//   5. Index the groups with a binary search per group start; range(prefix) is a binary search too.
//
// Everything the list points to is in the arena: it is valid until the arena's next reset (the render graph's, at the
// next frame's build). The inputs are CPU data and lookups, so the tests build lists without a device.

#include "rendering/extraction.h"
#include "rendering/geometry_arena.h"
#include "rendering/pipeline_variant_registry.h"
#include "rendering/sort_key.h"
#include "rendering/view.h"

#include "avis/enum_flags.h"
#include "avis/error.h"
#include "avis/scene/assets/material.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/flags.h"
#include "avis/scene/snapshot.h"

#include <Eigen/Core>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <type_traits>

namespace avis::graphics
{
    class linear_arena;
} // namespace avis::graphics

namespace avis::visualizer::rendering
{
    // One entry of a draw list (11 §4.1). Sorted on (key, value): values are unique, so the order is total.
    struct draw_item
    {
        std::uint64_t key = 0; // sort key (11 §4.2)
        std::uint32_t value = 0; // a draw item's draw_record index; a command item's argument offset

        friend constexpr auto operator<=>(const draw_item&, const draw_item&) = default;
    };

    // One per drawn instance per frame and view (11 §4.4); several items (camera, cascades) share it
    struct draw_record
    {
        std::uint32_t instance = 0; // index into extraction_result::instances (instance_data[])
        std::uint32_t mesh = 0; // index into extraction_result::meshes, the frame's mesh table
        std::uint32_t submesh = 0;
        avis::flags<scene::instance_flags> flags{ avis::empty }; // casts_shadows, selected, pickable, …
    };

    static_assert(std::is_trivially_copyable_v<draw_item> && std::is_trivially_copyable_v<draw_record>);

    // The alignment of a command item's arguments in draw_list::command_arguments() (11.9)
    inline constexpr std::size_t command_argument_alignment = 16;

    // What a draw list needs of a mesh the frame draws: its pipeline state and its submeshes' local bounds; and what
    // the range walker (11.9) needs to draw it: its place in the geometry arena
    struct draw_mesh
    {
        scene::mesh_topology topology = scene::mesh_topology::triangles;
        avis::flags<scene::vertex_attribute> attributes{ avis::empty };
        bool progressive = false;
        std::span<const scene::mesh_primitive> primitives; // the submeshes; their bounds are local (pixels when the
                                                           // instance is screen_aligned)
        geometry_range range; // the resident version's
    };

    // What a draw list needs of the material an instance draws with: the state of its pipeline set
    struct draw_material
    {
        scene::shading_model shading_model = scene::shading_model::lit;
        scene::alpha_mode alpha_mode = scene::alpha_mode::opaque;
        scene::blend_operation blend_operation = scene::blend_operation::normal;
        bool double_sided = false;

        friend bool operator==(const draw_material&, const draw_material&) = default;
    };

    // The state of a material slot's data (material_table)
    draw_material draw_material_of(const gpu::material_data& data) noexcept;

    // One shadow cascade's viewpoint (11 §7; computed by 11.12). Reverse-Z, like the camera: depth 1 at the near plane.
    struct cascade_viewpoint
    {
        Eigen::Matrix4f view_from_world = Eigen::Matrix4f::Identity(); // the light's view, looking down −Z
        Eigen::Matrix4f projection = Eigen::Matrix4f::Identity(); // clip from view, orthographic
    };

    struct draw_list_lookup
    {
        // The state of the material an instance draws with: the slot it got in extraction
        std::function<draw_material(scene::material_handle)> material;
        // The pipeline set of a key (pipeline_variant_registry::find). An error skips the instance.
        std::function<result<std::uint32_t>(const pipeline_set_key&)> pipeline_set;
    };

    struct draw_list_sources
    {
        const scene::snapshot& snapshot; // the snapshot extracted: each instance's submesh, layer, flags, distance
        const extraction_result& extraction;
        std::span<const draw_mesh> meshes; // by instance_data::mesh, parallel to extraction.meshes
        std::span<const cascade_viewpoint> cascades; // at most maximum_shadow_cascades; empty: no shadow items
        std::span<const draw_item> commands; // the renderer's command items (11 §4.7), copied into the list
        std::span<const std::byte> command_arguments; // their arguments, at each item's value (draw_commands, 11.9)
        draw_list_lookup lookup;
        std::uint32_t opaque_depth_bands = 16; // render.draw_sorting.opaque_depth_bands (11 §4.3)
    };

    // A run of items sharing a key group (fullscreen layer, viewport, viewport layer, translucency)
    struct draw_group
    {
        key_group group;
        std::uint32_t first = 0; // index of its first item
        std::uint32_t count = 0;
    };

    struct draw_list_statistics
    {
        std::size_t records = 0;
        std::size_t items = 0; // draw and command items
        std::size_t camera_items = 0;
        std::array<std::size_t, maximum_shadow_cascades> cascade_items{};
        std::size_t command_items = 0;
        std::size_t groups = 0;

        std::size_t culled_frustum = 0; // records outside the camera frustum
        std::size_t culled_distance = 0; // records beyond their maximum_view_distance
        std::size_t skipped_invalid = 0; // failing a debug check (screen_aligned in world) or a bad mesh or submesh
        std::size_t skipped_no_pipeline = 0; // the pipeline set lookup failed
    };

    // A view's sorted items, their records and their groups. A value type of spans into the arena it was built in.
    class draw_list
    {
    public:
        draw_list() = default;

        std::span<const draw_item> items() const noexcept
        {
            return items_;
        }

        std::span<const draw_record> records() const noexcept
        {
            return records_;
        }

        // In key order; they cover items() without gaps
        std::span<const draw_group> groups() const noexcept
        {
            return groups_;
        }

        // The command items' arguments, at each item's value; 16-byte aligned (11.9)
        std::span<const std::byte> command_arguments() const noexcept
        {
            return command_arguments_;
        }

        // The items whose key has `prefix`, or between the two ends of `range`; empty when there are none
        std::span<const draw_item> range(sort_key_prefix prefix) const noexcept;
        std::span<const draw_item> range(key_range keys) const noexcept;

        const draw_list_statistics& statistics() const noexcept
        {
            return statistics_;
        }

    private:
        friend draw_list build_draw_list(
            const view_description& view, const draw_list_sources& sources, graphics::linear_arena& arena);

        std::span<const draw_item> items_;
        std::span<const draw_record> records_;
        std::span<const draw_group> groups_;
        std::span<const std::byte> command_arguments_;
        draw_list_statistics statistics_;
    };

    // Steps 1 to 5 for `view`. Instances that fail a debug check (a screen_aligned instance in the world layer, 11
    // §3.4) or whose mesh or submesh isn't in `meshes` get no record; instances whose pipeline set lookup fails get no
    // item.
    draw_list
        build_draw_list(const view_description& view, const draw_list_sources& sources, graphics::linear_arena& arena);

    // Step 4: sorts on (key, value). The order is total, so it is the order std::ranges::sort gives; lists of 512 items
    // or more use an LSD radix sort (11 §4.5) with its scratch array in `arena`, skipping the bytes every item shares.
    void sort_draw_items(std::span<draw_item> items, graphics::linear_arena& arena);

    // Frustum tests, exposed for tests. Planes are a·x + b·y + c·z + d ≥ 0 inside, normalised; a degenerate plane (the
    // far plane of an infinite projection) is always passed.
    class frustum
    {
    public:
        // The frustum of clip_from_world, with D3D's clip volume: −w ≤ x, y ≤ w and 0 ≤ z ≤ w
        explicit frustum(const Eigen::Matrix4f& clip_from_world) noexcept;

        // False when the box is entirely outside one plane (conservative: may keep a box near a corner). An empty box
        // intersects nothing.
        bool intersects(const scene::aabb& box) const noexcept;

        // The same for the box of `centre` ± `half_size`
        bool intersects(const Eigen::Vector3f& centre, const Eigen::Vector3f& half_size) const noexcept
        {
            for (std::size_t plane = 0; plane < plane_count; ++plane)
            {
                const float distance =
                    x_[plane] * centre.x() + y_[plane] * centre.y() + z_[plane] * centre.z() + w_[plane];
                const float radius = absolute_x_[plane] * half_size.x() + absolute_y_[plane] * half_size.y() +
                                     absolute_z_[plane] * half_size.z();
                if (distance + radius < 0.0f)
                {
                    return false;
                }
            }
            return true;
        }

    private:
        static constexpr std::size_t plane_count = 6;

        // The planes by component, and the absolute values of their normals, for the box tests
        std::array<float, plane_count> x_{}, y_{}, z_{}, w_{};
        std::array<float, plane_count> absolute_x_{}, absolute_y_{}, absolute_z_{};
    };

    // Whether a screen_aligned mesh anchored at `anchor` can cover part of the view: the anchor is in front of the near
    // plane, and inside the clip volume widened at its depth by `pixel_bounds` (the mesh's local bounds, in pixels,
    // scaled by device_pixel_ratio). The widening is symmetric, so it is conservative for either y direction.
    bool screen_aligned_visible(
        const Eigen::Matrix4f& clip_from_world,
        const Eigen::Vector3f& anchor,
        const scene::aabb& pixel_bounds,
        float width,
        float height,
        float device_pixel_ratio) noexcept;
} // namespace avis::visualizer::rendering

#endif
