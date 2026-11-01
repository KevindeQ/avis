#ifndef AVIS_SCENE_SNAPSHOT_H
#define AVIS_SCENE_SNAPSHOT_H

// What the simulation thread hands the renderer (docs/design/08-scene-management.md §6, §9.8; 11 §3.1): generic render
// data only, in a double buffer. scene::make_snapshot fills a snapshot; snapshot_buffer passes it to the one reader.
//
// Snapshots hold only values and shared_ptr<const …> to immutable asset versions, so a snapshot_reference is safe to
// read on any thread while it lives.

#include "avis/enum_flags.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/flags.h"
#include "avis/scene/types.h"
#include "avis/simulation/time.h"

#include <Eigen/Core>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace avis::scene
{
    // One primitive of one entity's mesh (08 §6.1, 11 §3.1)
    struct render_instance
    {
        scene::object_id object_id = 0; // identity::id: stable, opaque to the renderer
        mesh_handle mesh;
        std::uint32_t submesh = 0; // the primitive's index in the mesh's current version
        material_handle material; // invalid: the renderer's default material
        Eigen::Matrix4f world_previous =
            Eigen::Matrix4f::Identity(); // at t_{k-1}; = world_current with history_invalid
        Eigen::Matrix4f world_current = Eigen::Matrix4f::Identity(); // at t_k
        draw_layer layer = draw_layer::world;
        std::uint8_t layer_priority = 0;
        avis::flags<instance_flags> flags = default_instance_flags;
        float maximum_view_distance = 0.0f; // metres from the camera; 0 = unlimited (08 §8.1)
    };

    struct light_instance
    {
        object_id id = 0;
        light_kind kind = light_kind::directional;
        color light_color;
        float intensity = 1.0f; // lux (directional), candela (point, spot)
        float range = 0.0f;
        float inner_cone_angle = 0.0f;
        float outer_cone_angle = 0.0f;
        Eigen::Matrix4f world = Eigen::Matrix4f::Identity(); // −Z is the light direction
        bool casts_shadows = false;
    };

    // Set by the Visualizer from the scenario's visualization settings, through scene::set_environment
    struct environment_settings
    {
        Eigen::Vector3f sun_direction = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
        float sun_illuminance = 100'000.0f; // lux
        float sky_turbidity = 2.5f;
        float ambient_intensity = 1.0f;
    };

    struct versioned_mesh_entry
    {
        mesh_handle handle;
        versioned_mesh mesh;
    };

    // The material's textures are in snapshot::textures, once each however many materials share them
    struct versioned_material_entry
    {
        material_handle handle;
        versioned_material material;
    };

    struct versioned_texture_entry
    {
        texture_handle handle;
        versioned_texture texture;
    };

    struct snapshot
    {
        // Set by the caller of make_snapshot. A republish keeps the step fields of the snapshot it replaces.
        simulation::step_index step = 0;
        simulation::simulation_duration time_previous{ 0 };
        simulation::simulation_duration time_current{ 0 };
        simulation::time_anchor anchor; // for interpolation (03 §5.2)

        // Set by make_snapshot. Instances are sorted by id, then submesh; lights by id.
        std::vector<render_instance> instances;
        std::vector<light_instance> lights;
        environment_settings environment;
        std::uint64_t scene_version = 0; // increments on structural change (UI refresh hint)

        // Set by snapshot_buffer::publish: increments on every publish, step commits and republishes (03 §4.4)
        std::uint64_t publish_sequence = 0;

        // Every asset that `instances` reference, once each, sorted by handle, with its current version (08 §5.2).
        // `textures` holds the textures of every material in `materials`.
        std::vector<versioned_mesh_entry> meshes;
        std::vector<versioned_material_entry> materials;
        std::vector<versioned_texture_entry> textures;

        // Empties the vectors, keeping their capacity; drops the asset references
        void clear() noexcept;
    };

    class snapshot_buffer;

    // Read access to the front slot as it was when acquired; the RAII hold that keeps the writer off it. Move-only.
    // Hold it only while extracting (08 §6.2): the writer's second begin_write after it waits for its release.
    class snapshot_reference
    {
    public:
        snapshot_reference() noexcept = default; // empty
        snapshot_reference(snapshot_reference&& other) noexcept;
        snapshot_reference& operator=(snapshot_reference&& other) noexcept;
        snapshot_reference(const snapshot_reference&) = delete;
        snapshot_reference& operator=(const snapshot_reference&) = delete;
        ~snapshot_reference(); // releases the slot

        const snapshot& operator*() const noexcept;
        const snapshot* operator->() const noexcept;
        explicit operator bool() const noexcept; // false before the first publish

    private:
        friend class snapshot_buffer;
        snapshot_reference(snapshot_buffer& buffer, std::uint32_t slot) noexcept;
        void reset() noexcept;

        snapshot_buffer* buffer_ = nullptr;
        std::uint32_t slot_ = 0;
    };

    // Two slots, front and back (08 §6.2). One writer, the simulation thread, and one reader, the renderer's frame
    // chain. A short spinlock guards the front index and the reader's hold.
    class snapshot_buffer
    {
    public:
        snapshot_buffer() = default;
        snapshot_buffer(const snapshot_buffer&) = delete;
        snapshot_buffer& operator=(const snapshot_buffer&) = delete;
        ~snapshot_buffer(); // asserts that no reference is alive

        // Writer. Returns the back slot, cleared with its capacity kept. Waits while the reader still holds that
        // slot, which is short when the reader holds its reference only while extracting; debug builds warn past
        // 2 ms. Exactly one publish() per begin_write(), a debug check.
        snapshot& begin_write();

        // Writer. Makes the back slot the front one and sets its publish_sequence to publish_sequence() + 1.
        void publish();

        // Reader. Never waits. Empty before the first publish; a second reference while one is alive is a debug
        // check, and gives an empty one.
        snapshot_reference acquire();

        // Of the last publish; 0 before the first
        std::uint64_t publish_sequence() const noexcept;

    private:
        friend class snapshot_reference;

        // The spinlock: an atomic_flag with a pause loop
        void lock() noexcept;
        void unlock() noexcept;
        void release(std::uint32_t slot) noexcept;

        std::array<snapshot, 2> slots_;
        std::atomic_flag lock_; // clear on construction (C++20)

        // Under the lock
        std::uint32_t front_ = 0;
        bool published_ = false;
        std::uint32_t held_ = 0; // one bit per slot the reader holds

        // Writer only
        bool writing_ = false; // between begin_write and publish

        std::atomic<std::uint64_t> publish_sequence_{ 0 };
    };
} // namespace avis::scene

#endif
