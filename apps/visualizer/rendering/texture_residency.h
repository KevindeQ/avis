#ifndef AVIS_VISUALIZER_RENDERING_TEXTURE_RESIDENCY_H
#define AVIS_VISUALIZER_RENDERING_TEXTURE_RESIDENCY_H

// GPU residency of textures (docs/design/11-renderer.md §3.3, §3.6, docs/tasks/11-renderer.md §11.4). The same path as
// meshes (residency_cache.h): each new version of a snapshot texture gets its own RHI texture with a bindless SRV, its
// mips go through the upload queue, and superseded versions are destroyed once no frame can read them.
//
// Decoded PNG and JPEG data (texture_data::generate_mips) uploads only mip 0. Its chain is then generated on the GPU in
// the frame after the upload's flush: the renderer adds the passes for mip_requests() to that frame's graph and calls
// mips_recorded() once the graph is built. The version is current from the next frame on, so the bindless reads of
// later frames come after the generation on the graphics queue. Until a version is current, materials draw with the
// previous version or with a default texture.
//
// Four 1 × 1 defaults stand in for textures that aren't resident: white, black, a flat normal and a magenta "missing"
// texture for one that can't be made resident at all.
//
// Per frame, on the prepare stage: update(snapshot, frame), the upload queue's flush, after_flush(), the mip passes,
// mips_recorded(). retire(completed) when begin_frame recycles a context.
//
// Not thread-safe: the frame chain is the only user. find's pointers are valid until the next update, after_flush,
// mips_recorded or retire.

#include "avis/error.h"
#include "avis/graphics/upload_ticket.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/types.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/mesh_data.h"
#include "avis/scene/assets/texture_data.h"
#include "avis/scene/snapshot.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace avis::graphics
{
    class upload_queue;
} // namespace avis::graphics

namespace avis::visualizer::rendering
{
    // A texture version on the GPU
    struct resident_texture
    {
        std::uint64_t version = 0;
        rhi::texture_handle texture;
        rhi::bindless_index srv; // every mip
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mip_levels = 1;
        rhi::format format = rhi::format::rgba8_unorm;
        rhi::synchronization_point ready_after; // the copy-queue point of its upload
        std::uint64_t last_used_frame = 0;
    };

    enum class default_texture : std::uint8_t
    {
        white, // (1, 1, 1, 1): base colour, metallic-roughness and occlusion stand-in
        black, // (0, 0, 0, 1): emissive stand-in
        flat_normal, // (0.5, 0.5, 1, 1): normal stand-in
        missing // (1, 0, 1, 1): a texture that can't be made resident
    };

    inline constexpr std::size_t default_texture_count = 4;

    // What a material resolves a texture handle against
    enum class texture_status : std::uint8_t
    {
        unknown, // not in the latest snapshot's textures
        pending, // uploading, or waiting for its mips
        resident, // find() returns its current version
        failed // its newest version can't be made resident (format, size): draw it as missing
    };

    // A texture whose chain the frame's graph generates (scene_renderer adds the passes): mip 0 is uploaded, mips
    // 1 to mip_levels − 1 are to be written
    struct mip_request
    {
        scene::texture_handle handle;
        std::uint64_t version = 0;
        rhi::texture_handle texture;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mip_levels = 1;
        bool srgb = false;
        rhi::synchronization_point ready_after;
    };

    struct texture_residency_statistics
    {
        std::size_t textures = 0; // handles with a current version
        std::size_t resident_versions = 0; // current and superseded versions with a GPU texture
        std::size_t pending = 0; // uploading or waiting for mips
        std::size_t failed = 0; // versions that can't be made resident
        std::uint64_t uploaded_bytes = 0;
        std::uint64_t failed_uploads = 0; // attempts deferred to a later frame
        std::uint64_t mip_chains_generated = 0;
    };

    // The number of mips of a full chain down to 1 × 1
    std::uint32_t full_mip_count(std::uint32_t width, std::uint32_t height) noexcept;

    // The RHI format of a scene texture format
    rhi::format to_rhi_format(scene::texture_format format) noexcept;

    class texture_residency
    {
    public:
        // Creates the default textures and queues their uploads (they're flushed with the first frame). The queue must
        // outlive the residency.
        static result<std::unique_ptr<texture_residency>>
            create(rhi::resources resources, graphics::upload_queue& uploads, rhi::synchronization synchronization);

        // Destroys every texture; the RHI's deferred release keeps those frames still use
        ~texture_residency();

        texture_residency(const texture_residency&) = delete;
        texture_residency& operator=(const texture_residency&) = delete;

        // Before the flush: creates and uploads the snapshot's new texture versions. A full staging ring retries the
        // upload next frame; a texture the RHI refuses is marked failed and not retried.
        void update(const scene::snapshot& snapshot, std::uint64_t frame_index);

        // After the flush: resolves the tickets (nullopt is a debug check), makes the uploaded versions that need no
        // mips current, collects the mip requests of this frame, and marks the current versions of the latest
        // snapshot's textures as drawn by this frame
        void after_flush();

        // The textures whose chain this frame's graph generates, in a stable order
        std::span<const mip_request> mip_requests() const noexcept
        {
            return mip_requests_;
        }

        // The frame's graph is built with the passes of mip_requests(): those versions are current from the next update
        // on. Without this call, the requests are made again next frame.
        void mips_recorded();

        const resident_texture* find(scene::texture_handle texture) const;
        texture_status status(scene::texture_handle texture) const;

        // The bindless SRV of a default texture
        rhi::bindless_index default_srv(default_texture which) const noexcept;
        const resident_texture& default_resident(default_texture which) const noexcept;

        // A frame draws a material slot that references this version: it lives until that frame retires
        void mark_used(scene::texture_handle texture, std::uint64_t version, std::uint64_t frame_index) noexcept;

        // The newest ready_after of the defaults and of the current versions the frame of the last after_flush marked
        rhi::synchronization_point frame_ready_after() const noexcept
        {
            return frame_ready_after_;
        }

        // Destroys versions that are superseded, failed, abandoned or no longer in the latest snapshot, once
        // last_used_frame ≤ completed and their copies are complete
        void retire(std::uint64_t completed_frame_index);

        texture_residency_statistics statistics() const;

        // Counts the changes of a version's status that change how materials resolve it: becoming ready, or failing.
        // The material table resolves them at its next update; the reuse shortcut waits for that (11 §3.5).
        std::uint64_t status_changes() const noexcept
        {
            return status_changes_;
        }

        // For tests: the versions held for a texture, oldest first
        std::vector<std::uint64_t> versions(scene::texture_handle texture) const;

    private:
        struct handle_hash
        {
            std::size_t operator()(scene::texture_handle handle) const noexcept
            {
                return std::hash<std::uint64_t>{}((std::uint64_t{ handle.index } << 32) | handle.generation);
            }
        };

        enum class entry_state : std::uint8_t
        {
            uploading, // data held until upload_texture succeeds, then a ticket until after_flush
            needs_mips, // uploaded; its chain is generated by a frame's graph
            ready
        };

        struct entry
        {
            resident_texture texture;
            entry_state state = entry_state::uploading;
            std::shared_ptr<const scene::texture_data> data; // until the upload is queued
            std::optional<graphics::upload_ticket> ticket; // queued, waiting for after_flush
            bool generate_mips = false;
            bool srgb = false;
            bool abandoned = false; // superseded before it became current: destroyed, never drawn
        };

        struct record
        {
            std::vector<entry> entries; // by version, oldest first
            bool in_latest_snapshot = false;
            std::uint64_t failed_version = 0; // the newest version the RHI refused
            std::optional<std::chrono::steady_clock::time_point> last_warning;
        };

        texture_residency(
            rhi::resources resources, graphics::upload_queue& uploads, rhi::synchronization synchronization) noexcept;

        // Queues the upload of `item`'s data; false when the ring was full or the queue refused it
        bool upload(scene::texture_handle handle, record& owner, entry& item);
        void warn(scene::texture_handle handle, record& owner, std::uint64_t version, const std::string& reason);

        static const entry* newest_ready(const record& owner) noexcept;
        static entry* newest_ready(record& owner) noexcept;

        rhi::resources resources_;
        graphics::upload_queue& uploads_;
        rhi::synchronization synchronization_;
        std::array<resident_texture, default_texture_count> defaults_;
        std::array<std::optional<graphics::upload_ticket>, default_texture_count> default_tickets_;
        std::unordered_map<scene::texture_handle, record, handle_hash> records_;
        std::vector<mip_request> mip_requests_;
        std::uint64_t frame_index_ = 0;
        rhi::synchronization_point frame_ready_after_;
        std::uint64_t uploaded_bytes_ = 0;
        std::uint64_t failed_uploads_ = 0;
        std::uint64_t mip_chains_generated_ = 0;
        std::uint64_t status_changes_ = 0;
    };
} // namespace avis::visualizer::rendering

#endif
