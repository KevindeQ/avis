#include "avis/graphics/upload_queue.h"

#include "avis/detail/check.h"
#include "avis/detail/safe_error.h"
#include "avis/graphics/error.h"
#include "avis/rhi/formats.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <utility>
#include <vector>

namespace avis::graphics
{
    namespace detail
    {
        // A copy queued for the next flush (17 §7.1)
        struct pending_copy
        {
            enum class kind : std::uint8_t
            {
                buffer,
                texture
            };

            kind type = kind::buffer;
            rhi::buffer_handle source; // the ring or a dedicated upload buffer
            std::uint64_t source_offset = 0;
            rhi::buffer_handle destination_buffer;
            std::uint64_t destination_offset = 0;
            rhi::texture_handle destination_texture;
            rhi::subresource destination_subresource;
            std::uint64_t size = 0; // buffers
        };

        struct in_flight_flush
        {
            std::uint64_t flush_index = 0;
            rhi::synchronization_point point;
        };

        // Ring space handed out and not yet free again, in ring order. The ring's tail only advances over a prefix of
        // reservations whose own flush is complete, so space reserved before a flush but queued after it is never
        // reused early.
        struct reservation
        {
            std::uint64_t end = 0; // ring counter past the reservation
            std::uint64_t flush_index = 0; // the flush carrying its copies; 0 while they aren't queued yet
        };

        // Where an upload's bytes go before the copy: ring space or a dedicated upload buffer
        struct staging_block
        {
            rhi::buffer_handle buffer;
            std::uint64_t offset = 0; // in buffer
            std::span<std::byte> bytes;
            std::uint64_t reservation_id = 0; // 0 for a dedicated buffer
        };

        struct upload_queue_state
        {
            std::uint32_t queue_id = 0;
            rhi::buffer_handle ring;
            std::span<std::byte> ring_bytes;
            std::uint64_t capacity = 0;

            mutable std::mutex mutex;
            // Ring counters grow forever; a position's offset in the ring is position % capacity
            std::uint64_t head = 0;
            std::uint64_t tail = 0;
            std::deque<reservation> reservations;
            std::uint64_t first_reservation_id = 1; // the id of reservations.front()
            std::uint64_t next_reservation_id = 1;
            std::vector<pending_copy> pending;
            std::vector<rhi::buffer_handle> dedicated; // carried by the next flush, destroyed after its submit
            std::deque<in_flight_flush> in_flight; // in flush order, which is completion order on the copy queue
            std::uint64_t next_flush_index = 1;
            std::uint64_t retired_flush_index = 0; // every flush up to this one is complete
            rhi::synchronization_point last_point;
        };
    } // namespace detail

    namespace
    {
        constexpr std::string_view logger_name = "graphics";
        constexpr std::uint64_t ring_granularity = std::uint64_t{ 64 } << 10;
        // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT: copy_buffer_to_texture reads from 512-byte aligned offsets
        constexpr std::uint64_t placement_alignment = 512;

        std::atomic<std::uint32_t> next_queue_id{ 1 };

        constexpr std::uint64_t align_up(const std::uint64_t value, const std::uint64_t alignment) noexcept
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        avis::error invalid(std::string message)
        {
            return make_error(error_kind::invalid_argument, std::move(message));
        }

        std::uint32_t mip_count(const rhi::texture_description& description) noexcept
        {
            if (description.mip_levels != 0)
            {
                return description.mip_levels;
            }
            std::uint32_t largest = std::max(description.width, description.height);
            if (description.dimension == rhi::texture_dimension::texture_3d)
            {
                largest = std::max(largest, description.depth_or_layers);
            }
            return static_cast<std::uint32_t>(std::bit_width(std::max(largest, 1u)));
        }

        std::uint32_t layer_count(const rhi::texture_description& description) noexcept
        {
            switch (description.dimension)
            {
            case rhi::texture_dimension::texture_3d:
                return 1;
            case rhi::texture_dimension::cube:
                return 6 * description.depth_or_layers;
            case rhi::texture_dimension::texture_1d:
            case rhi::texture_dimension::texture_2d:
                return description.depth_or_layers;
            }
            return description.depth_or_layers;
        }

        // One subresource of a texture upload: where the caller's rows are, and where they go in the staging block
        struct texture_piece
        {
            const subresource_data* data;
            rhi::copy_footprint footprint;
            std::uint64_t row_pitch; // the caller's
            std::uint64_t slice_pitch;
            std::uint64_t offset; // in the staging block
        };
    } // namespace

    result<std::unique_ptr<upload_queue>> upload_queue::create(
        rhi::resources resources,
        const rhi::command_lists command_lists,
        const rhi::queue<rhi::queue_type::copy> copy_queue,
        const rhi::synchronization synchronization,
        const upload_queue_configuration& configuration)
    {
        if (configuration.staging_ring_size == 0)
        {
            return std::unexpected(invalid("upload_queue: staging_ring_size is 0"));
        }
        const std::uint64_t capacity = align_up(configuration.staging_ring_size, ring_granularity);
        auto ring = resources.create_buffer(
            { .size = capacity,
              .usage = rhi::buffer_usage::copy_source,
              .memory = rhi::memory_type::upload,
              .debug_name = "upload queue ring" });
        if (!ring)
        {
            return std::unexpected(std::move(ring.error()));
        }

        auto state = std::make_unique<detail::upload_queue_state>();
        state->queue_id = next_queue_id.fetch_add(1, std::memory_order_relaxed);
        state->ring = *ring;
        state->ring_bytes = resources.mapped_bytes(*ring).first(capacity);
        state->capacity = capacity;

        return std::unique_ptr<upload_queue>{ new upload_queue{
            resources, command_lists, copy_queue, synchronization, std::move(state) } };
    }

    upload_queue::upload_queue(
        const rhi::resources resources,
        const rhi::command_lists command_lists,
        const rhi::queue<rhi::queue_type::copy> copy_queue,
        const rhi::synchronization synchronization,
        std::unique_ptr<detail::upload_queue_state> state) noexcept :
        resources_{ resources },
        command_lists_{ command_lists },
        copy_queue_{ copy_queue },
        synchronization_{ synchronization },
        state_{ std::move(state) }
    {}

    upload_queue::~upload_queue()
    {
        // The RHI's deferred release keeps them alive until in-flight copies are done
        for (const rhi::buffer_handle buffer : state_->dedicated)
        {
            resources_.destroy(buffer);
        }
        resources_.destroy(state_->ring);
    }

    result<upload_ticket> upload_queue::upload_buffer(
        const rhi::buffer_handle destination,
        const std::uint64_t destination_offset,
        const std::span<const std::byte> data)
    {
        if (!destination.is_valid())
        {
            return std::unexpected(invalid("upload_buffer: invalid destination buffer"));
        }
        const rhi::buffer_description& description = resources_.description(destination);
        if (description.memory != rhi::memory_type::gpu_only)
        {
            return std::unexpected(invalid(
                "upload_buffer: the destination isn't a gpu_only buffer (write upload buffers through mapped_bytes)"));
        }
        if (destination_offset > description.size || data.size() > description.size - destination_offset)
        {
            return std::unexpected(invalid(
                std::format(
                    "upload_buffer: {} bytes at offset {} don't fit a buffer of {} bytes",
                    data.size(),
                    destination_offset,
                    description.size)));
        }
        if (data.empty())
        {
            return upload_ticket{ .flush_index = 0, .queue_id = state_->queue_id };
        }

        try
        {
            auto block = stage(data.size());
            if (!block)
            {
                return std::unexpected(std::move(block.error()));
            }
            std::memcpy(block->bytes.data(), data.data(), data.size());

            const std::scoped_lock lock{ state_->mutex };
            state_->pending.push_back(
                { .type = detail::pending_copy::kind::buffer,
                  .source = block->buffer,
                  .source_offset = block->offset,
                  .destination_buffer = destination,
                  .destination_offset = destination_offset,
                  .size = data.size() });
            if (block->reservation_id != 0)
            {
                state_->reservations[block->reservation_id - state_->first_reservation_id].flush_index =
                    state_->next_flush_index;
            }
            else
            {
                state_->dedicated.push_back(block->buffer);
            }
            return upload_ticket{ .flush_index = state_->next_flush_index, .queue_id = state_->queue_id };
        }
        catch (...)
        {
            return std::unexpected(avis::detail::current_exception_error());
        }
    }

    result<upload_ticket> upload_queue::upload_texture(
        const rhi::texture_handle destination, const std::span<const subresource_data> data)
    {
        if (!destination.is_valid())
        {
            return std::unexpected(invalid("upload_texture: invalid destination texture"));
        }
        if (data.empty())
        {
            return upload_ticket{ .flush_index = 0, .queue_id = state_->queue_id };
        }

        try
        {
            const rhi::texture_description& description = resources_.description(destination);
            const std::uint32_t mips = mip_count(description);
            const std::uint32_t layers = layer_count(description);

            // Validate everything and lay the subresources out in one staging block, each 512-byte aligned
            std::vector<texture_piece> pieces;
            pieces.reserve(data.size());
            std::uint64_t total = 0;
            for (const subresource_data& subresource : data)
            {
                if (subresource.target.mip >= mips || subresource.target.layer >= layers)
                {
                    return std::unexpected(invalid(
                        std::format(
                            "upload_texture: subresource (mip {}, layer {}) is outside a texture of {} mips and {} "
                            "layers",
                            subresource.target.mip,
                            subresource.target.layer,
                            mips,
                            layers)));
                }
                const rhi::copy_footprint footprint = resources_.copyable_footprint(destination, subresource.target);
                const std::uint64_t rows = footprint.row_count;
                const std::uint64_t row_pitch = subresource.row_pitch != 0 ? subresource.row_pitch : footprint.row_size;
                const std::uint64_t slice_pitch =
                    subresource.slice_pitch != 0 ? subresource.slice_pitch : row_pitch * rows;
                if (row_pitch < footprint.row_size || (footprint.depth > 1 && slice_pitch < row_pitch * rows))
                {
                    return std::unexpected(invalid(
                        std::format(
                            "upload_texture: row pitch {} or slice pitch {} is shorter than a row ({} bytes) or a "
                            "slice",
                            row_pitch,
                            slice_pitch,
                            footprint.row_size)));
                }
                const std::uint64_t required =
                    (footprint.depth - 1) * slice_pitch + (rows - 1) * row_pitch + footprint.row_size;
                if (rows == 0 || subresource.bytes.size() < required)
                {
                    return std::unexpected(invalid(
                        std::format(
                            "upload_texture: subresource (mip {}, layer {}) has {} bytes, its rows need {}",
                            subresource.target.mip,
                            subresource.target.layer,
                            subresource.bytes.size(),
                            required)));
                }
                const std::uint64_t offset = align_up(total, placement_alignment);
                pieces.push_back({ &subresource, footprint, row_pitch, slice_pitch, offset });
                total = offset + footprint.total_size;
            }

            auto block = stage(total);
            if (!block)
            {
                return std::unexpected(std::move(block.error()));
            }

            // The caller's rows into the footprint's rows; depth slices are row_pitch × row_count apart
            for (const texture_piece& piece : pieces)
            {
                const rhi::copy_footprint& footprint = piece.footprint;
                for (std::uint64_t slice = 0; slice < footprint.depth; ++slice)
                {
                    for (std::uint64_t row = 0; row < footprint.row_count; ++row)
                    {
                        std::memcpy(
                            block->bytes.data() + piece.offset +
                                (slice * footprint.row_count + row) * footprint.row_pitch,
                            piece.data->bytes.data() + slice * piece.slice_pitch + row * piece.row_pitch,
                            footprint.row_size);
                    }
                }
            }

            const std::scoped_lock lock{ state_->mutex };
            for (const texture_piece& piece : pieces)
            {
                state_->pending.push_back(
                    { .type = detail::pending_copy::kind::texture,
                      .source = block->buffer,
                      .source_offset = block->offset + piece.offset,
                      .destination_texture = destination,
                      .destination_subresource = piece.data->target });
            }
            if (block->reservation_id != 0)
            {
                state_->reservations[block->reservation_id - state_->first_reservation_id].flush_index =
                    state_->next_flush_index;
            }
            else
            {
                state_->dedicated.push_back(block->buffer);
            }
            return upload_ticket{ .flush_index = state_->next_flush_index, .queue_id = state_->queue_id };
        }
        catch (...)
        {
            return std::unexpected(avis::detail::current_exception_error());
        }
    }

    result<detail::staging_block> upload_queue::stage(const std::uint64_t size)
    {
        detail::upload_queue_state& state = *state_;
        if (size > state.capacity / 2)
        {
            // Created before mutex_ is taken; destroyed right after the flush that carries it is submitted
            auto buffer = resources_.create_buffer(
                { .size = size,
                  .usage = rhi::buffer_usage::copy_source,
                  .memory = rhi::memory_type::upload,
                  .debug_name = "upload queue dedicated buffer" });
            if (!buffer)
            {
                return std::unexpected(std::move(buffer.error()));
            }
            return detail::staging_block{ .buffer = *buffer,
                                          .offset = 0,
                                          .bytes = resources_.mapped_bytes(*buffer).first(size),
                                          .reservation_id = 0 };
        }

        const std::scoped_lock lock{ state.mutex };
        // Reserves [start, start + size): aligned for texture copies, and wrapped to the ring's start (the padding
        // counted as used) when it doesn't fit before the end
        const auto reserve = [&state, size]() -> std::optional<std::uint64_t>
        {
            std::uint64_t start = align_up(state.head, placement_alignment);
            if (start % state.capacity + size > state.capacity)
            {
                start = (start / state.capacity + 1) * state.capacity;
            }
            const std::uint64_t end = start + size;
            if (end - state.tail > state.capacity)
            {
                return std::nullopt;
            }
            state.head = end;
            return start;
        };

        auto start = reserve();
        if (!start)
        {
            retire_locked();
            start = reserve();
        }
        if (!start)
        {
            return std::unexpected(make_error(
                error_kind::upload_queue_full,
                std::format(
                    "upload queue: no room for {} bytes in the {} byte staging ring; flush, or wait for earlier "
                    "flushes",
                    size,
                    state.capacity)));
        }

        state.reservations.push_back({ .end = state.head, .flush_index = 0 });
        const std::uint64_t offset = *start % state.capacity;
        return detail::staging_block{ .buffer = state.ring,
                                      .offset = offset,
                                      .bytes = state.ring_bytes.subspan(offset, size),
                                      .reservation_id = state.next_reservation_id++ };
    }

    void upload_queue::retire_locked()
    {
        detail::upload_queue_state& state = *state_;
        while (!state.in_flight.empty() && synchronization_.is_complete(state.in_flight.front().point))
        {
            state.retired_flush_index = state.in_flight.front().flush_index;
            state.in_flight.pop_front();
        }
        while (!state.reservations.empty() && state.reservations.front().flush_index != 0 &&
               state.reservations.front().flush_index <= state.retired_flush_index)
        {
            state.tail = state.reservations.front().end;
            state.reservations.pop_front();
            ++state.first_reservation_id;
        }
        if (state.reservations.empty())
        {
            state.tail = state.head;
        }
    }

    result<rhi::synchronization_point> upload_queue::flush()
    {
        detail::upload_queue_state& state = *state_;
        try
        {
            {
                const std::scoped_lock lock{ state.mutex };
                retire_locked();
                if (state.pending.empty())
                {
                    return state.last_point;
                }
            }

            auto list = command_lists_.acquire<rhi::queue_type::copy>("upload queue flush");
            if (!list)
            {
                return std::unexpected(std::move(list.error()));
            }

            std::vector<rhi::buffer_handle> dedicated;
            rhi::synchronization_point point;
            {
                // Held across submit, so flushes reach the copy queue in flush-index order
                const std::scoped_lock lock{ state.mutex };
                if (state.pending.empty())
                {
                    return state.last_point; // another flush took them; the list is discarded
                }
                for (const detail::pending_copy& copy : state.pending)
                {
                    if (copy.type == detail::pending_copy::kind::buffer)
                    {
                        list->copy_buffer(
                            copy.destination_buffer,
                            copy.destination_offset,
                            copy.source,
                            copy.source_offset,
                            copy.size);
                    }
                    else
                    {
                        list->copy_buffer_to_texture(
                            copy.destination_texture, copy.destination_subresource, copy.source, copy.source_offset);
                    }
                }
                auto submitted = copy_queue_.submit(std::span{ &*list, 1 });
                if (!submitted)
                {
                    return std::unexpected(std::move(submitted.error())); // the copies stay queued
                }
                point = *submitted;
                state.in_flight.push_back({ .flush_index = state.next_flush_index, .point = point });
                state.last_point = point;
                ++state.next_flush_index;
                state.pending.clear();
                dedicated.swap(state.dedicated);
            }

            for (const rhi::buffer_handle buffer : dedicated)
            {
                resources_.destroy(buffer);
            }
            return point;
        }
        catch (...)
        {
            return std::unexpected(avis::detail::current_exception_error());
        }
    }

    std::optional<rhi::synchronization_point> upload_queue::point(const upload_ticket ticket) const noexcept
    {
        if (!AVIS_CHECK(
                logger_name,
                ticket.queue_id == state_->queue_id,
                "upload_queue::point: the ticket was issued by another upload queue"))
        {
            return std::nullopt;
        }
        if (ticket.flush_index == 0)
        {
            return rhi::synchronization_point{};
        }

        const std::scoped_lock lock{ state_->mutex };
        if (ticket.flush_index >= state_->next_flush_index)
        {
            return std::nullopt; // its flush hasn't happened yet
        }
        for (const detail::in_flight_flush& flush : state_->in_flight)
        {
            if (flush.flush_index == ticket.flush_index)
            {
                return flush.point;
            }
        }
        return rhi::synchronization_point{}; // retired
    }
} // namespace avis::graphics
