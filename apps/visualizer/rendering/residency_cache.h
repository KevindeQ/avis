#ifndef AVIS_VISUALIZER_RENDERING_RESIDENCY_CACHE_H
#define AVIS_VISUALIZER_RENDERING_RESIDENCY_CACHE_H

// GPU residency of meshes (docs/design/11-renderer.md §3.6, docs/tasks/11-renderer.md §11.3, D-41, D-103). The
// renderer owns every GPU copy; the asset cache only the immutable CPU versions.
//
// Each frame, on the frame chain's prepare stage:
//   1. update(snapshot, frame): queues the uploads of the versions that aren't resident, into new arena ranges
//   2. the caller flushes the upload queue
//   3. after_flush(): resolves the tickets into ready_after points, makes the fully uploaded versions current, and
//      marks the version each mesh draws this frame
//   4. find(handle) gives the version to draw; frame_ready_after() the point the arena import waits on
// and, when begin_frame recycles a context, retire(completed) frees the ranges no frame can read any more.
//
// A version that can't be uploaded this frame (the staging ring is full, or the arena has no room) is retried next
// frame, from the stream that failed, and the previous version keeps drawing. A mesh with no resident version isn't
// drawn. The renderer never waits for an upload.
//
// Not thread-safe: the frame chain is the only user. find's pointers are valid until the next update, after_flush or
// retire.

#include "rendering/geometry_arena.h"

#include "avis/enum_flags.h"
#include "avis/graphics/upload_ticket.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/synchronization.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/bounds.h"
#include "avis/scene/snapshot.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace avis::graphics
{
    class upload_queue;
} // namespace avis::graphics

namespace avis::visualizer::rendering
{
    // A mesh version in the arena, ready to draw
    struct resident_mesh
    {
        std::uint64_t version = 0;
        geometry_range range;
        std::uint32_t vertex_count = 0;
        std::uint32_t index_count = 0;
        avis::flags<scene::vertex_attribute> attributes{ avis::empty };
        scene::mesh_topology topology = scene::mesh_topology::triangles;
        bool progressive = false;
        scene::aabb bounds; // local, of every primitive
        std::vector<scene::mesh_primitive> primitives; // the submeshes; their offsets are relative to the range
        rhi::synchronization_point ready_after; // the copy-queue point of its uploads; the first use waits on it
        std::uint64_t last_used_frame = 0; // the last frame that drew it
    };

    struct residency_statistics
    {
        std::size_t meshes = 0; // handles with a resident version
        std::size_t resident_versions = 0; // ready versions, current and superseded
        std::size_t pending_uploads = 0; // versions with uploads queued or still to queue
        std::uint64_t uploaded_bytes = 0; // queued for upload since creation
        std::uint64_t failed_uploads = 0; // upload attempts deferred to a later frame since creation
        geometry_arena_statistics arena;
    };

    class residency_cache
    {
    public:
        // The arena and the queue must outlive the cache
        residency_cache(geometry_arena& arena, graphics::upload_queue& uploads, rhi::synchronization synchronization);

        // Frees every range at once: the caller has waited for the GPU, or destroys the arena next
        ~residency_cache();

        residency_cache(const residency_cache&) = delete;
        residency_cache& operator=(const residency_cache&) = delete;

        // Prepare stage, before the upload queue's flush: walks snapshot.meshes and queues the uploads of every
        // (handle, version) that isn't resident. The new version becomes current in after_flush.
        void update(const scene::snapshot& snapshot, std::uint64_t frame_index);

        // After the flush: resolves the tickets queued so far (one resolving to nullopt is a debug check: the flush
        // hasn't happened), makes the fully uploaded versions current, and marks the version each mesh of the last
        // snapshot draws with the frame of the last update
        void after_flush();

        // The newest resident version, or nullptr when the mesh has none
        const resident_mesh* find(scene::mesh_handle mesh) const;

        // The newest ready_after of the versions the frame of the last after_flush draws: the arena import waits on it
        rhi::synchronization_point frame_ready_after() const noexcept
        {
            return frame_ready_after_;
        }

        // Frame `completed_frame_index` and every earlier one are complete on the GPU. Frees the ranges of versions
        // that are superseded, or whose mesh isn't in the latest snapshot, once no completed-or-earlier frame is
        // still to finish with them: last_used_frame ≤ completed, and their copies are complete.
        void retire(std::uint64_t completed_frame_index);

        residency_statistics statistics() const;

        // For tests: the versions held for a mesh, ready or uploading, oldest first
        std::vector<std::uint64_t> versions(scene::mesh_handle mesh) const;

    private:
        struct handle_hash
        {
            std::size_t operator()(scene::mesh_handle handle) const noexcept
            {
                return std::hash<std::uint64_t>{}((std::uint64_t{ handle.index } << 32) | handle.generation);
            }
        };

        // A version whose streams are queued one by one; the CPU data is held until every stream is queued
        struct upload_state
        {
            std::shared_ptr<const scene::mesh_data> data;
            std::uint32_t next_stream = 0; // the next stream to queue; past the last when all are queued
            std::vector<graphics::upload_ticket> unresolved; // queued, waiting for after_flush
            bool abandoned = false; // superseded before it was complete: freed, never drawn
        };

        struct entry
        {
            resident_mesh mesh;
            std::optional<upload_state> upload; // empty once ready
        };

        struct record
        {
            std::vector<entry> entries; // by version, oldest first
            bool in_latest_snapshot = false;
            std::optional<std::chrono::steady_clock::time_point> last_warning;
        };

        // Queues the remaining streams of `item`; false when one couldn't be queued (retried next frame)
        bool upload_streams(scene::mesh_handle handle, record& owner, entry& item);
        void warn(scene::mesh_handle handle, record& owner, std::uint64_t version, const std::string& reason);

        static const entry* newest_ready(const record& owner) noexcept;
        static entry* newest_ready(record& owner) noexcept;

        geometry_arena& arena_;
        graphics::upload_queue& uploads_;
        rhi::synchronization synchronization_;
        std::unordered_map<scene::mesh_handle, record, handle_hash> records_;
        std::uint64_t frame_index_ = 0; // of the last update
        rhi::synchronization_point frame_ready_after_;
        std::uint64_t uploaded_bytes_ = 0;
        std::uint64_t failed_uploads_ = 0;
    };
} // namespace avis::visualizer::rendering

#endif
