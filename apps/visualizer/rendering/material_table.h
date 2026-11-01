#ifndef AVIS_VISUALIZER_RENDERING_MATERIAL_TABLE_H
#define AVIS_VISUALIZER_RENDERING_MATERIAL_TABLE_H

// The renderer's materials on the GPU (docs/design/11-renderer.md §3.3, §3.6, docs/tasks/11-renderer.md §11.4): one
// persistent structured buffer of material_data (shared_types.hlsli), indexed by a material's slot. Switching
// material is a root-constant change.
//
// A new material version gets a new slot, written through the upload queue, with its texture fields resolved against
// the resident textures: the current version's bindless SRV, a default stand-in while a texture is still uploading
// (white for base colour, metallic-roughness and occlusion, a flat normal, black emissive), magenta for one that
// can't be made resident, and 0 ("none") for no texture. When a resolution changes, because a texture became resident
// or got a new version, the material is written again, into a new slot too: frames still in flight read the old
// slot, so a slot is never rewritten in place. A slot is current once its write is flushed; until then the previous
// one draws, or the default material in slot 0.
//
// Slots are freed by retirement, like geometry ranges: superseded or no longer in the latest snapshot, last drawn by a
// completed frame, and their copy complete. The textures a drawn slot references are marked as used by the frame too.
//
// Per frame, on the prepare stage after texture_residency::update: update(snapshot, frame), the upload queue's flush,
// after_flush() after texture_residency::after_flush(). retire(completed) when begin_frame recycles a context.
//
// Not thread-safe: the frame chain is the only user.

#include "rendering/texture_residency.h"

#include "avis/error.h"
#include "avis/graphics/upload_ticket.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/types.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/material.h"
#include "avis/scene/snapshot.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_types.hlsli>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace avis::graphics
{
    class upload_queue;
} // namespace avis::graphics

namespace avis::visualizer::rendering
{
    // The material fields that take a texture, in material_data order
    enum class material_texture : std::uint8_t
    {
        base_color,
        metallic_roughness,
        normal,
        occlusion,
        emissive
    };

    inline constexpr std::size_t material_texture_count = 5;

    // A texture version a slot's data references (none: the field has no texture, or a default stands in)
    struct referenced_texture
    {
        scene::texture_handle handle;
        std::uint64_t version = 0;

        friend bool operator==(const referenced_texture&, const referenced_texture&) = default;
    };

    // A material version's slot
    struct resident_material
    {
        std::uint64_t version = 0;
        std::uint32_t slot = 0;
        gpu::material_data data{};
        std::array<referenced_texture, material_texture_count> textures{};
        rhi::synchronization_point ready_after;
        std::uint64_t last_used_frame = 0;
    };

    struct material_table_statistics
    {
        std::uint32_t capacity = 0; // slots, the default one included
        std::uint32_t slots_used = 0; // the default one included
        std::size_t materials = 0; // handles with a current slot
        std::size_t pending_writes = 0;
        std::uint64_t writes = 0; // slots written since creation, the default one included
        std::uint64_t rewrites = 0; // writes for a version already written: a texture resolution changed
        std::uint64_t failed_writes = 0; // attempts deferred to a later frame (ring full, no free slot)
    };

    // The material_data of a description, with its texture fields resolved against `textures` as the class
    // comment describes. `referenced` receives the resident versions it points to.
    gpu::material_data make_material_data(
        const scene::material_description& description,
        const texture_residency& textures,
        std::array<referenced_texture, material_texture_count>& referenced);

    class material_table
    {
    public:
        static constexpr std::uint32_t default_slot = 0; // white, lit, opaque: instances without a material
        static constexpr std::uint32_t maximum_slots = 1u << 17; // the material field of the sort key (11 §4.2)

        // Creates the buffer of `capacity` slots (at most maximum_slots) and queues the default material's write. The
        // queue and the textures must outlive the table.
        static result<std::unique_ptr<material_table>> create(
            rhi::resources resources,
            graphics::upload_queue& uploads,
            rhi::synchronization synchronization,
            texture_residency& textures,
            std::uint32_t capacity = maximum_slots);

        // Destroys the buffer; the RHI's deferred release keeps it while frames use it
        ~material_table();

        material_table(const material_table&) = delete;
        material_table& operator=(const material_table&) = delete;

        // Before the flush, after texture_residency::update: writes the slots of new versions and of materials whose
        // texture resolution changed
        void update(const scene::snapshot& snapshot, std::uint64_t frame_index);

        // After the flush and texture_residency::after_flush: resolves the tickets (nullopt is a debug check), makes
        // the written slots current, and marks the slot each material of the latest snapshot draws, and its textures
        void after_flush();

        // The slot to draw `material` with: its current slot, or default_slot
        std::uint32_t slot(scene::material_handle material) const;

        // The current slot of `material`, or nullptr
        const resident_material* find(scene::material_handle material) const;

        // A structured buffer of material_data, for the passes' bindless reads
        rhi::buffer_handle buffer() const noexcept
        {
            return buffer_;
        }

        rhi::bindless_index buffer_srv() const noexcept
        {
            return buffer_srv_;
        }

        // The newest ready_after of the default slot and of the slots the frame of the last after_flush marked
        rhi::synchronization_point frame_ready_after() const noexcept
        {
            return frame_ready_after_;
        }

        // Frees the slots that are superseded, abandoned or no longer in the latest snapshot, once last_used_frame ≤
        // completed and their write is complete
        void retire(std::uint64_t completed_frame_index);

        material_table_statistics statistics() const;

        // For tests: the versions held for a material, oldest first (a rewritten version appears twice)
        std::vector<std::uint64_t> versions(scene::material_handle material) const;

    private:
        struct handle_hash
        {
            std::size_t operator()(scene::material_handle handle) const noexcept
            {
                return std::hash<std::uint64_t>{}((std::uint64_t{ handle.index } << 32) | handle.generation);
            }
        };

        struct entry
        {
            resident_material material;
            bool written = false; // the upload is queued (the ring had room)
            std::optional<graphics::upload_ticket> ticket; // queued, waiting for after_flush
            bool ready = false;
            bool abandoned = false; // replaced before it became current: freed, never drawn
        };

        struct record
        {
            std::vector<entry> entries; // oldest first
            bool in_latest_snapshot = false;
            std::optional<std::chrono::steady_clock::time_point> last_warning;
        };

        material_table(
            rhi::resources resources,
            graphics::upload_queue& uploads,
            rhi::synchronization synchronization,
            texture_residency& textures,
            rhi::buffer_handle buffer,
            std::uint32_t capacity) noexcept;

        std::optional<std::uint32_t> allocate_slot() noexcept;
        void free_slot(std::uint32_t slot) noexcept;
        // Queues the write of `item`'s data into its slot; false when the ring was full
        bool write(scene::material_handle handle, record& owner, entry& item);
        void warn(scene::material_handle handle, record& owner, std::uint64_t version, const std::string& reason);

        static const entry* newest_ready(const record& owner) noexcept;
        static entry* newest_ready(record& owner) noexcept;

        rhi::resources resources_;
        graphics::upload_queue& uploads_;
        rhi::synchronization synchronization_;
        texture_residency& textures_;
        rhi::buffer_handle buffer_;
        rhi::bindless_index buffer_srv_;
        std::uint32_t capacity_ = 0;

        std::vector<std::uint32_t> free_slots_; // freed slots, reused before new ones
        std::uint32_t next_slot_ = default_slot + 1; // the first never-used slot
        entry default_entry_;
        std::unordered_map<scene::material_handle, record, handle_hash> records_;

        std::uint64_t frame_index_ = 0;
        rhi::synchronization_point frame_ready_after_;
        std::uint64_t writes_ = 0;
        std::uint64_t rewrites_ = 0;
        std::uint64_t failed_writes_ = 0;
    };
} // namespace avis::visualizer::rendering

#endif
