#ifndef AVIS_VISUALIZER_RENDERING_PROGRESSIVE_LOD_H
#define AVIS_VISUALIZER_RENDERING_PROGRESSIVE_LOD_H

// Progressive meshes (docs/design/11-renderer.md §9.3, docs/tasks/11-renderer.md §11.18). A progressive mesh's vertices
// are in random order, so any prefix is a uniform subsample. Per view and frame, each drawn instance of one draws
//   n = clamp(k · projected_area_px, n_minimum, vertex_count)
// vertices in the camera viewport, and max(1, ⌊shadow_lod_scale · n⌋) in each shadow cascade it casts into. k, in
// vertices per pixel, is the largest value for which the view's total (camera and shadow vertices) stays within the
// vertex budget, found each frame by a bisection over the exact integer total. Under the budget, every instance draws
// whole: the budget is the only limit, so lifting it (a screenshot, 11 §12.1) draws full density.
//
// Rate limit: an instance drawn in the view's previous frame (keyed by object_id) moves its n by at most
// maximum_change (±25%) of the previous n, so the density doesn't pop when the camera jumps. The budget wins over the
// rate limit: when the lower bounds alone exceed it, they're scaled down. A history_invalid instance, or one the view
// didn't draw in its previous frame, starts free.
//
// The LOD applies to point primitives and to non-indexed lines and triangles (rounded down to whole segments and
// triangles by the walker). An indexed line or triangle primitive's vertex prefix isn't a prefix of its primitives, so
// it draws whole; it still gets one instance per run.

#include "rendering/draw_lists.h"
#include "rendering/view.h"

#include "avis/scene/types.h"

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace avis::graphics
{
    class linear_arena;
} // namespace avis::graphics

namespace avis::visualizer::rendering
{
    struct progressive_lod_parameters
    {
        std::uint64_t vertex_budget = 20'000'000; // render.progressive.vertex_budget, or the view's override
        float shadow_lod_scale = 0.25f; // render.progressive.shadow_lod_scale, in (0, 1]
        std::uint32_t minimum_vertices = 256; // n_minimum (or the whole mesh when it has fewer)
        float maximum_change = 0.25f; // of the previous frame's n, per frame
    };

    // One drawn instance of a progressive primitive, in a view
    struct progressive_instance
    {
        scene::object_id object_id = 0;
        std::uint32_t vertex_count = 0; // the primitive's
        float projected_area = 0.0f; // pixels: the bounding sphere's projection, at most the view's area
        bool camera = false; // it has a camera item
        std::uint32_t shadow_viewports = 0; // the cascades it has items in
        bool history_invalid = false; // created this tick: no rate limit (11 §3.1)
    };

    // What an instance draws: in the camera viewport, and in each shadow viewport. 0 means the primitive's whole vertex
    // count (not a progressive LOD); a computed count is at least 1.
    struct progressive_vertex_counts
    {
        std::uint32_t camera = 0;
        std::uint32_t shadow = 0;

        friend bool operator==(const progressive_vertex_counts&, const progressive_vertex_counts&) = default;
    };

    struct progressive_lod_statistics
    {
        std::size_t instances = 0;
        std::uint64_t budget = 0;
        double vertices_per_pixel = 0.0; // the frame's k; infinite when every instance draws whole
        std::uint64_t camera_vertices = 0;
        std::uint64_t shadow_vertices = 0; // over every cascade
        std::uint64_t full_vertices = 0; // what the instances would draw whole (camera and shadow)
        std::size_t rate_limited = 0; // instances whose n the rate limit clamped
        bool minimums_scaled = false; // the lower bounds exceeded the budget and were scaled down

        std::uint64_t vertices() const noexcept
        {
            return camera_vertices + shadow_vertices;
        }
    };

    // The shadow viewports' count of a camera count n
    std::uint32_t shadow_vertex_count(std::uint32_t camera_count, float shadow_lod_scale) noexcept;

    // The projected area, in pixels, of the sphere at `centre` of `radius` seen by `camera` in a view of `size`: π
    // times the square of its projected radius, at most the view's area (the whole view when the camera is inside it)
    float projected_area(
        const Eigen::Vector3f& centre, float radius, const camera& camera, graphics::texture_extent size) noexcept;

    // One view's LOD state: each object's n of the view's previous frame
    class progressive_lod
    {
    public:
        // Fills `counts` (parallel to `instances`) and remembers this frame's n of each object. Objects not in
        // `instances` are forgotten.
        progressive_lod_statistics update(
            std::span<const progressive_instance> instances,
            const progressive_lod_parameters& parameters,
            std::span<progressive_vertex_counts> counts);

        // Forgets every object: the next frame isn't rate-limited (a camera cut or a scene reload)
        void reset() noexcept
        {
            previous_.clear();
        }

        std::size_t size() const noexcept
        {
            return previous_.size();
        }

    private:
        std::unordered_map<scene::object_id, std::uint32_t> previous_;
        std::unordered_map<scene::object_id, std::uint32_t> next_; // kept for its buckets
    };

    // A view's LOD, by draw record
    struct progressive_draw_counts
    {
        std::span<const progressive_vertex_counts> by_record; // in the arena; zeros for records that aren't progressive
        progressive_lod_statistics statistics;
    };

    // Gathers the progressive instances of `list` (records whose mesh is progressive and that have items), computes
    // their counts with `lod`, and lays them out by record in `arena`. `meshes` and `instances` are the ones the list
    // was built with.
    progressive_draw_counts compute_progressive_lod(
        const view_description& view,
        const draw_list& list,
        std::span<const draw_mesh> meshes,
        std::span<const gpu::instance_data> instances,
        const progressive_lod_parameters& parameters,
        progressive_lod& lod,
        graphics::linear_arena& arena);
} // namespace avis::visualizer::rendering

#endif
