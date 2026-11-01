#include "rendering/residency_cache.h"

#include "avis/detail/check.h"
#include "avis/graphics/upload_queue.h"
#include "avis/log.h"

#include <Eigen/Core>

#include <array>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "renderer";
        constexpr auto warning_interval = std::chrono::seconds{ 1 };

        static_assert(sizeof(Eigen::Vector3f) == position_stride, "positions upload as tightly packed f32x3");
        static_assert(sizeof(std::array<std::uint16_t, 2>) == attribute_stride);
        static_assert(sizeof(std::array<std::uint8_t, 4>) == attribute_stride);
        static_assert(sizeof(std::uint32_t) == index_stride);

        // The streams in upload order; the vertex streams in the order of layout_vertex_streams
        enum class stream : std::uint32_t
        {
            position,
            normal,
            tangent,
            uv0,
            color0,
            indices,
            count
        };

        template<typename T>
        std::span<const std::byte> bytes_of(const std::vector<T>& values) noexcept
        {
            return std::as_bytes(std::span{ values });
        }

        // Octahedral u16x2 with the bitangent sign in bit 0 of the second component (geometry_arena.h)
        std::vector<std::array<std::uint16_t, 2>> encode_tangents(const scene::mesh_data& data)
        {
            std::vector<std::array<std::uint16_t, 2>> encoded = data.tangents;
            for (std::size_t vertex = 0; vertex < encoded.size(); ++vertex)
            {
                const bool negative = vertex < data.tangent_signs.size() && data.tangent_signs[vertex] < 0;
                encoded[vertex][1] = static_cast<std::uint16_t>((encoded[vertex][1] & ~1u) | (negative ? 1u : 0u));
            }
            return encoded;
        }

        std::string describe(const scene::mesh_handle handle, const std::uint64_t version)
        {
            return std::format("mesh {}:{} version {}", handle.index, handle.generation, version);
        }
    } // namespace

    residency_cache::residency_cache(
        geometry_arena& arena, graphics::upload_queue& uploads, const rhi::synchronization synchronization) :
        arena_{ arena },
        uploads_{ uploads },
        synchronization_{ synchronization }
    {}

    residency_cache::~residency_cache()
    {
        for (auto& [handle, owner] : records_)
        {
            for (const entry& item : owner.entries)
            {
                arena_.free(item.mesh.range);
            }
        }
    }

    void residency_cache::update(const scene::snapshot& snapshot, const std::uint64_t frame_index)
    {
        frame_index_ = frame_index;
        for (auto& [handle, owner] : records_)
        {
            owner.in_latest_snapshot = false;
        }

        for (const scene::versioned_mesh_entry& listed : snapshot.meshes)
        {
            if (listed.mesh.version == 0 || !listed.mesh.data)
            {
                continue; // a stale handle: nothing to draw
            }
            record& owner = records_[listed.handle];
            owner.in_latest_snapshot = true;

            // Already resident, or uploading: carry on with the streams a full ring left last frame. An abandoned
            // upload of this version (its mesh left the snapshot and came back) doesn't count: it starts again.
            entry* held = nullptr;
            for (entry& item : owner.entries)
            {
                if (item.mesh.version == listed.mesh.version && !(item.upload && item.upload->abandoned))
                {
                    held = &item;
                }
            }
            if (held != nullptr)
            {
                if (held->upload && !held->upload->abandoned &&
                    held->upload->next_stream < static_cast<std::uint32_t>(stream::count))
                {
                    upload_streams(listed.handle, owner, *held);
                }
                continue;
            }

            // A new version: an older one still uploading will never draw, so it's abandoned (and freed by retire
            // once its copies are done)
            for (entry& item : owner.entries)
            {
                if (item.upload && !item.upload->abandoned)
                {
                    item.upload->abandoned = true;
                    item.upload->data.reset();
                }
            }

            const scene::mesh_data& data = *listed.mesh.data;
            const auto vertex_count = static_cast<std::uint32_t>(data.positions.size());
            const vertex_stream_layout layout = layout_vertex_streams(vertex_count, data.attributes);
            const std::uint64_t index_size = std::uint64_t{ data.indices.size() } * index_stride;
            const std::optional<geometry_range> range = arena_.allocate(layout, index_size);
            if (!range)
            {
                ++failed_uploads_;
                const geometry_arena_statistics arena = arena_.statistics();
                const bool vertices = arena_.last_exhausted() == arena_exhausted::vertices;
                warn(
                    listed.handle,
                    owner,
                    listed.mesh.version,
                    std::format(
                        "the geometry arena's {} buffer has no free block of {} bytes ({} of {} bytes used, the "
                        "largest free block is {} bytes; render.geometry.arena_mib)",
                        vertices ? "vertex" : "index",
                        vertices ? layout.size : index_size,
                        vertices ? arena.vertex_used : arena.index_used,
                        vertices ? arena.vertex_capacity : arena.index_capacity,
                        vertices ? arena.largest_free_vertex_block : arena.largest_free_index_block));
                continue;
            }

            entry item;
            item.mesh.version = listed.mesh.version;
            item.mesh.range = *range;
            item.mesh.vertex_count = vertex_count;
            item.mesh.index_count = static_cast<std::uint32_t>(data.indices.size());
            item.mesh.attributes = data.attributes;
            item.mesh.topology = data.topology;
            item.mesh.progressive = data.progressive;
            item.mesh.bounds = data.bounds;
            item.mesh.primitives = data.primitives;
            item.mesh.last_used_frame = frame_index;
            item.upload = upload_state{ .data = listed.mesh.data };
            owner.entries.push_back(std::move(item));
            upload_streams(listed.handle, owner, owner.entries.back());
        }
    }

    bool residency_cache::upload_streams(const scene::mesh_handle handle, record& owner, entry& item)
    {
        upload_state& upload = *item.upload;
        const scene::mesh_data& data = *upload.data;
        const geometry_range& range = item.mesh.range;

        for (; upload.next_stream < static_cast<std::uint32_t>(stream::count); ++upload.next_stream)
        {
            std::vector<std::array<std::uint16_t, 2>> tangents; // lives until upload_buffer has copied it
            std::span<const std::byte> bytes;
            std::uint32_t offset = vertex_stream_offsets::absent_stream;
            rhi::buffer_handle destination = arena_.vertex_buffer();
            switch (static_cast<stream>(upload.next_stream))
            {
            case stream::position:
                bytes = bytes_of(data.positions);
                offset = range.streams.position;
                break;
            case stream::normal:
                bytes = bytes_of(data.normals);
                offset = range.streams.normal;
                break;
            case stream::tangent:
                if (range.streams.tangent != vertex_stream_offsets::absent_stream)
                {
                    tangents = encode_tangents(data);
                    bytes = bytes_of(tangents);
                    offset = range.streams.tangent;
                }
                break;
            case stream::uv0:
                bytes = bytes_of(data.uv0);
                offset = range.streams.uv0;
                break;
            case stream::color0:
                bytes = bytes_of(data.color0);
                offset = range.streams.color0;
                break;
            case stream::indices:
                bytes = bytes_of(data.indices);
                offset = range.index_size > 0 ? range.index_offset : vertex_stream_offsets::absent_stream;
                destination = arena_.index_buffer();
                break;
            case stream::count:
                break;
            }
            if (offset == vertex_stream_offsets::absent_stream || bytes.empty())
            {
                continue;
            }

            result<graphics::upload_ticket> ticket = uploads_.upload_buffer(destination, offset, bytes);
            if (!ticket)
            {
                ++failed_uploads_;
                warn(handle, owner, item.mesh.version, std::format("upload failed: {}", ticket.error()));
                return false;
            }
            upload.unresolved.push_back(*ticket);
            uploaded_bytes_ += bytes.size();
            item.mesh.last_used_frame = frame_index_; // its copies run with this frame's flush
        }

        // Every stream is in the staging ring: the CPU data isn't needed any more (11 §3.6)
        upload.data.reset();
        return true;
    }

    void residency_cache::warn(
        const scene::mesh_handle handle, record& owner, const std::uint64_t version, const std::string& reason)
    {
        const auto now = std::chrono::steady_clock::now();
        if (owner.last_warning && now - *owner.last_warning < warning_interval)
        {
            return;
        }
        owner.last_warning = now;
        log::get(logger_name)
            ->warn(
                "residency: {} not made resident this frame: {}. Retried next frame; the previous version keeps "
                "drawing.",
                describe(handle, version),
                reason);
    }

    void residency_cache::after_flush()
    {
        frame_ready_after_ = {};
        for (auto& [handle, owner] : records_)
        {
            for (entry& item : owner.entries)
            {
                if (!item.upload)
                {
                    continue;
                }
                upload_state& upload = *item.upload;
                std::erase_if(
                    upload.unresolved,
                    [&](const graphics::upload_ticket ticket)
                    {
                        const std::optional<rhi::synchronization_point> point = uploads_.point(ticket);
                        if (!AVIS_CHECK(
                                logger_name,
                                point.has_value(),
                                "residency: an upload ticket resolved to nullopt, so its flush hasn't happened (11 "
                                "§3.6): flush the upload queue before after_flush"))
                        {
                            return false; // kept for the next after_flush
                        }
                        item.mesh.ready_after = rhi::latest(item.mesh.ready_after, *point);
                        return true;
                    });
                if (!upload.abandoned && upload.next_stream == static_cast<std::uint32_t>(stream::count) &&
                    upload.unresolved.empty())
                {
                    item.upload.reset(); // ready: the newest resident version from now on
                }
            }

            // The version this frame draws
            if (owner.in_latest_snapshot)
            {
                if (entry* current = newest_ready(owner))
                {
                    current->mesh.last_used_frame = frame_index_;
                    frame_ready_after_ = rhi::latest(frame_ready_after_, current->mesh.ready_after);
                }
            }
        }
    }

    const resident_mesh* residency_cache::find(const scene::mesh_handle mesh) const
    {
        const auto found = records_.find(mesh);
        if (found == records_.end())
        {
            return nullptr;
        }
        const entry* current = newest_ready(found->second);
        return current == nullptr ? nullptr : &current->mesh;
    }

    void residency_cache::retire(const std::uint64_t completed_frame_index)
    {
        for (auto record_it = records_.begin(); record_it != records_.end();)
        {
            record& owner = record_it->second;
            const entry* current = owner.in_latest_snapshot ? newest_ready(owner) : nullptr;
            const std::optional<std::uint64_t> current_version =
                current != nullptr ? std::optional{ current->mesh.version } : std::nullopt;

            std::erase_if(
                owner.entries,
                [&](entry& item)
                {
                    if (!item.upload && item.mesh.version == current_version)
                    {
                        return false;
                    }
                    if (item.upload)
                    {
                        if (!owner.in_latest_snapshot && !item.upload->abandoned)
                        {
                            // Its mesh left the snapshot before the upload finished
                            item.upload->abandoned = true;
                            item.upload->data.reset();
                        }
                        if (!item.upload->abandoned || !item.upload->unresolved.empty())
                        {
                            return false; // still to become current, or copies not flushed yet
                        }
                    }
                    if (item.mesh.last_used_frame > completed_frame_index)
                    {
                        return false;
                    }
                    // A version that never drew had nothing wait for its copies
                    if (item.mesh.ready_after.value != 0 && !synchronization_.is_complete(item.mesh.ready_after))
                    {
                        return false;
                    }
                    arena_.free(item.mesh.range);
                    return true;
                });

            if (!owner.in_latest_snapshot && owner.entries.empty())
            {
                record_it = records_.erase(record_it);
            }
            else
            {
                ++record_it;
            }
        }
    }

    residency_statistics residency_cache::statistics() const
    {
        residency_statistics statistics{ .uploaded_bytes = uploaded_bytes_,
                                         .failed_uploads = failed_uploads_,
                                         .arena = arena_.statistics() };
        for (const auto& [handle, owner] : records_)
        {
            bool resident = false;
            for (const entry& item : owner.entries)
            {
                if (!item.upload)
                {
                    resident = true;
                    ++statistics.resident_versions;
                }
                else if (!item.upload->abandoned)
                {
                    ++statistics.pending_uploads;
                }
            }
            if (resident)
            {
                ++statistics.meshes;
            }
        }
        return statistics;
    }

    std::vector<std::uint64_t> residency_cache::versions(const scene::mesh_handle mesh) const
    {
        std::vector<std::uint64_t> held;
        if (const auto found = records_.find(mesh); found != records_.end())
        {
            for (const entry& item : found->second.entries)
            {
                held.push_back(item.mesh.version);
            }
        }
        return held;
    }

    const residency_cache::entry* residency_cache::newest_ready(const record& owner) noexcept
    {
        for (auto item = owner.entries.rbegin(); item != owner.entries.rend(); ++item)
        {
            if (!item->upload)
            {
                return &*item;
            }
        }
        return nullptr;
    }

    residency_cache::entry* residency_cache::newest_ready(record& owner) noexcept
    {
        return const_cast<entry*>(newest_ready(std::as_const(owner)));
    }
} // namespace avis::visualizer::rendering
