#ifndef AVIS_VISUALIZER_RENDERING_EXTRACTION_H
#define AVIS_VISUALIZER_RENDERING_EXTRACTION_H

// Extraction and interpolation (docs/design/11-renderer.md §3.5, docs/tasks/11-renderer.md §11.5, D-42, D-56, A-15):
// the prepare stage turns the snapshot into the frame's GPU arrays, after residency has made its versions current.
//   1. Interpolation: t_render and alpha from the snapshot's time anchor (03 §5).
//   2. Instances: instance_data[] with the interpolated world (translation lerp, rotation slerp, scale lerp of the
//      decomposed matrices), the previous rendered world, the normal matrix, the mesh, the material slot, the flags and
//      the object id. Invisible instances, and instances whose mesh has no resident version, are skipped.
//   3. Previous transforms: the world each object_id was drawn with in the previous rendered frame, kept in
//      extraction_state. They are not the previous tick's transforms.
//   4. Lights: light_data[] from the snapshot's lights, at most maximum_lights; the first directional one is the sun.
//
// CPU only: residency is reached through extraction_lookup, so the tests run it on hand-built snapshots.

#include "avis/scene/snapshot.h"
#include "avis/scene/types.h"
#include "avis/simulation/time.h"

#include <Eigen/Core>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <shared_types.hlsli>
#include <unordered_map>
#include <vector>

namespace avis::visualizer::rendering
{
    // 11 §6.2: lights past this are dropped with a warning
    inline constexpr std::size_t maximum_lights = 4096;

    // Where the frame sits between the snapshot's two ticks (03 §5)
    struct frame_interpolation
    {
        simulation::simulation_duration t_render{}; // in [time_previous, time_current]
        float alpha = 1.0f; // in [0, 1]; 1 while paused or stopped (rate 0)
    };

    // t_render = clamp(t_base + (wall_now − wall_base) · rate, t_{k−1}, t_k), alpha = (t_render − t_{k−1}) / (t_k −
    // t_{k−1}). At rate 0, or when the two ticks are at the same time, alpha is 1 and t_render is t_k.
    frame_interpolation interpolate(const scene::snapshot& snapshot, std::chrono::steady_clock::time_point wall_now);

    // The world matrix at `alpha` between two worlds: their TRS decompositions (scene::transform::from_affine)
    // interpolated, translation and scale linearly, rotation by slerp on the shortest arc. alpha 0 and 1 give the
    // matrices themselves, exactly.
    Eigen::Matrix4f interpolate_world(const Eigen::Matrix4f& previous, const Eigen::Matrix4f& current, float alpha);

    // What extraction needs from residency (11 §3.6)
    struct extraction_lookup
    {
        // The version of `mesh` the frame draws, or nullopt when it has no resident version: the instance is skipped
        std::function<std::optional<std::uint64_t>(scene::mesh_handle)> mesh_version;
        // The material slot to draw `material` with (the default slot when it has none yet)
        std::function<std::uint32_t(scene::material_handle)> material_slot;
    };

    // A mesh the frame draws: instance_data::mesh indexes extraction_result::meshes
    struct frame_mesh
    {
        scene::mesh_handle handle;
        std::uint64_t version = 0;

        friend bool operator==(const frame_mesh&, const frame_mesh&) = default;
    };

    struct extraction_result
    {
        frame_interpolation interpolation;
        std::uint64_t publish_sequence = 0; // of the snapshot extracted

        // One per drawn instance, in snapshot order
        std::vector<gpu::instance_data> instances;
        std::vector<std::uint32_t> instance_sources; // the snapshot::instances index of each instance
        std::vector<frame_mesh> meshes; // in order of first use

        std::vector<gpu::light_data> lights; // in snapshot order
        std::optional<std::uint32_t> sun; // index in `lights` of the first directional light

        // True when every instance's world_previous equals its world: the instance array has no motion, so a later
        // frame of the same snapshot and alpha may reuse it (11 §3.5)
        bool stationary = true;

        std::size_t skipped_invisible = 0; // instances without instance_flags::visible
        std::size_t skipped_not_resident = 0; // instances whose mesh has no resident version
        std::size_t skipped_invalid = 0; // instances failing a debug check (screen_aligned in the world layer)
        std::size_t dropped_lights = 0; // past maximum_lights

        // Empties the arrays, keeping their capacity
        void clear() noexcept;
    };

    // Kept across frames by the caller: the world each object was drawn with in the previous rendered frame
    class extraction_state
    {
    public:
        // The previous rendered world of `id`, or nullptr when it wasn't drawn in the previous rendered frame
        const Eigen::Matrix4f* previous_world(scene::object_id id) const;

        // Objects drawn in the previous rendered frame
        std::size_t size() const noexcept
        {
            return previous_worlds_.size();
        }

        // Forgets every object: the next frame has no motion (a scene reload)
        void clear() noexcept;

    private:
        friend void extract(
            const scene::snapshot& snapshot,
            std::chrono::steady_clock::time_point wall_now,
            extraction_state& state,
            const extraction_lookup& lookup,
            extraction_result& result);

        std::unordered_map<scene::object_id, Eigen::Matrix4f> previous_worlds_;
        std::unordered_map<scene::object_id, Eigen::Matrix4f> next_worlds_; // built by extract, then swapped in
        std::optional<std::chrono::steady_clock::time_point> last_light_warning_;
    };

    // Steps 1 to 4 into `result` (cleared first, its capacity kept); `state` then holds this frame's worlds. An object
    // gets previous = current when it wasn't drawn in the previous rendered frame, or when the snapshot marks a pop-in
    // or a jump (instance_flags::history_invalid, or world_previous == world_current). A screen_aligned instance in the
    // world layer fails a debug check and is skipped (11 §3.4).
    void extract(
        const scene::snapshot& snapshot,
        std::chrono::steady_clock::time_point wall_now,
        extraction_state& state,
        const extraction_lookup& lookup,
        extraction_result& result);

    extraction_result extract(
        const scene::snapshot& snapshot,
        std::chrono::steady_clock::time_point wall_now,
        extraction_state& state,
        const extraction_lookup& lookup);
} // namespace avis::visualizer::rendering

#endif
