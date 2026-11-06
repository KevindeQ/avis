#ifndef AVIS_VISUALIZER_RENDERING_READBACK_RING_H
#define AVIS_VISUALIZER_RENDERING_READBACK_RING_H

// A small ring of readback buffers for results the renderer reads back from a frame (docs/tasks/11-renderer.md §11.19,
// §11.20): picks, the pixel inspector's texels, the cluster statistics. A slot holds one result from the frame that
// requests it until the result is taken; the ring creates the slots' buffers when it needs them, up to its capacity.
// The frame's passes copy into the slot's buffer, the renderer marks the slot submitted with the frame's
// submitted_frame, and take() hands over the bytes once its submissions are complete on the GPU.

#include "avis/error.h"
#include "avis/graphics/render_graph.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace avis::visualizer::rendering
{
    template<typename Payload>
    class readback_ring
    {
    public:
        struct pending
        {
            Payload payload;
            std::uint64_t frame_index = 0;
            std::uint64_t sequence = 0; // the order of the requests
            std::optional<graphics::submitted_frame> submitted; // set once the frame's work is submitted
        };

        struct slot
        {
            rhi::buffer_handle buffer; // `bytes`, memory_type::readback
            std::optional<pending> request;
        };

        readback_ring(
            rhi::resources resources, const std::size_t capacity, const std::uint64_t bytes, std::string debug_name) :
            resources_{ resources },
            capacity_{ capacity },
            bytes_{ bytes },
            debug_name_{ std::move(debug_name) }
        {}

        ~readback_ring()
        {
            // Deferred by the RHI until the GPU is done with them
            for (const slot& entry : slots_)
            {
                resources_.destroy(entry.buffer);
            }
        }

        readback_ring(const readback_ring&) = delete;
        readback_ring& operator=(const readback_ring&) = delete;

        // A free slot for a request of frame `frame_index`, or nullptr when every slot is in use. Fails when a new
        // slot's buffer can't be created.
        result<slot*> acquire(const std::uint64_t frame_index, Payload payload)
        {
            auto free = std::ranges::find_if(slots_, [](const slot& entry) { return !entry.request; });
            if (free == slots_.end())
            {
                if (slots_.size() >= capacity_)
                {
                    return nullptr;
                }
                auto buffer = resources_.create_buffer(
                    { .size = bytes_,
                      .usage = rhi::buffer_usage::copy_destination,
                      .memory = rhi::memory_type::readback,
                      .debug_name = debug_name_ });
                if (!buffer)
                {
                    return std::unexpected{ std::move(buffer).error() };
                }
                slots_.push_back({ .buffer = *buffer });
                free = slots_.end() - 1;
            }
            free->request =
                pending{ .payload = std::move(payload), .frame_index = frame_index, .sequence = reserve_sequence() };
            return &*free;
        }

        // A place in the order of the requests, for a result that needs no slot
        std::uint64_t reserve_sequence() noexcept
        {
            return next_sequence_++;
        }

        // The frame's requests were submitted
        void submitted(const std::uint64_t frame_index, const graphics::submitted_frame& frame)
        {
            for (slot& entry : slots_)
            {
                if (entry.request && entry.request->frame_index == frame_index && !entry.request->submitted)
                {
                    entry.request->submitted = frame;
                }
            }
        }

        // The frame's requests that weren't submitted are dropped: the frame failed
        void drop(const std::uint64_t frame_index) noexcept
        {
            for (slot& entry : slots_)
            {
                if (entry.request && entry.request->frame_index == frame_index && !entry.request->submitted)
                {
                    entry.request.reset();
                }
            }
        }

        // Calls consume(const pending&, std::span<const std::byte>) for each request that `select(payload)` accepts and
        // whose submissions are complete, in request order, and frees its slot
        template<typename Select, typename Consume>
        void take(const rhi::synchronization synchronization, Select&& select, Consume&& consume)
        {
            std::vector<slot*> complete;
            for (slot& entry : slots_)
            {
                if (!entry.request || !entry.request->submitted || !select(entry.request->payload))
                {
                    continue;
                }
                const bool done = std::ranges::all_of(
                    entry.request->submitted->last_submissions(),
                    [synchronization](const rhi::synchronization_point point)
                    { return synchronization.is_complete(point); });
                if (done)
                {
                    complete.push_back(&entry);
                }
            }
            std::ranges::sort(complete, {}, [](const slot* entry) { return entry->request->sequence; });
            for (slot* entry : complete)
            {
                consume(std::as_const(*entry->request), resources_.mapped_bytes(entry->buffer));
                entry->request.reset();
            }
        }

        std::size_t capacity() const noexcept
        {
            return capacity_;
        }

    private:
        rhi::resources resources_;
        std::size_t capacity_;
        std::uint64_t bytes_;
        std::string debug_name_;
        std::vector<slot> slots_;
        std::uint64_t next_sequence_ = 0;
    };
} // namespace avis::visualizer::rendering

#endif
