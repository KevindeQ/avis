#ifndef AVIS_VISUALIZER_RENDERING_FRAME_DATA_H
#define AVIS_VISUALIZER_RENDERING_FRAME_DATA_H

// Pass 0, upload_frame_data (docs/design/11-renderer.md §2.3, §3.5, docs/tasks/11-renderer.md §11.5): the frame's
// instance_data, instance_indices and light_data arrays go into persistent, ring-indexed gpu_only buffers that the
// passes read bindlessly.
//
// Each array has a frame_buffer_ring of F slots (F = frames in flight). A frame writes a slot that no recorded or
// in-flight frame reads: one whose last frame is complete on the GPU. A slot too small for the array is reallocated,
// doubling its size, which is safe for the same reason. The bytes go through a staging upload buffer per frame
// context (reused once the context is recycled, so once frame N − F is complete), and a copy pass of the graph copies
// them on the copy queue. The graph places the barriers: the destination buffers are imported every frame.
//
// The reuse shortcut (11 §3.5): a frame that reuses the previous extraction writes no instance or light slot. It reads
// the slots the previous frame wrote, which stay pinned: marking them as used by this frame keeps them from being
// handed out again until this frame retires too.
//
// Not thread-safe: the frame chain is the only user.

#include "avis/error.h"
#include "avis/graphics/buffer.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_types.hlsli>
#include <span>
#include <string>
#include <vector>

namespace avis::graphics
{
    class frame_token;
    class render_graph;
} // namespace avis::graphics

namespace avis::visualizer::rendering
{
    // Persistent gpu_only structured buffers for one per-frame array, used round-robin by frames in flight
    class frame_buffer_ring
    {
    public:
        // `slot_count` slots, created on first use with room for at least `minimum_elements` elements of `stride` bytes
        frame_buffer_ring(
            rhi::resources resources,
            std::string name,
            std::uint32_t stride,
            std::uint32_t slot_count,
            std::uint32_t minimum_elements);

        // Destroys the buffers; the RHI's deferred release keeps them while frames use them
        ~frame_buffer_ring();

        frame_buffer_ring(const frame_buffer_ring&) = delete;
        frame_buffer_ring& operator=(const frame_buffer_ring&) = delete;

        // A slot with room for `elements` elements that no frame after `completed_frame_index` uses, marked as used by
        // `frame_index`. Grows a slot that is too small by doubling (its buffer is replaced). When every slot is in
        // use, which takes more frames in flight than slots, a slot is added. Fails with the RHI's error.
        result<std::uint32_t>
            acquire(std::size_t elements, std::uint64_t frame_index, std::uint64_t completed_frame_index);

        // `slot` is read by `frame_index` too: it isn't handed out before that frame is complete
        void mark_used(std::uint32_t slot, std::uint64_t frame_index) noexcept;

        rhi::buffer_handle buffer(std::uint32_t slot) const noexcept
        {
            return slots_[slot].buffer;
        }

        rhi::bindless_index srv(std::uint32_t slot) const noexcept
        {
            return slots_[slot].srv;
        }

        // In elements
        std::size_t capacity(std::uint32_t slot) const noexcept
        {
            return slots_[slot].capacity;
        }

        std::uint32_t slot_count() const noexcept
        {
            return static_cast<std::uint32_t>(slots_.size());
        }

        // Buffers created since construction, first creations included
        std::uint64_t allocations() const noexcept
        {
            return allocations_;
        }

    private:
        struct slot
        {
            rhi::buffer_handle buffer;
            rhi::bindless_index srv;
            std::size_t capacity = 0; // elements; 0 before the buffer exists
            std::uint64_t last_used_frame = 0;
        };

        result<void> reallocate(slot& target, std::size_t elements);

        rhi::resources resources_;
        std::string name_;
        std::uint32_t stride_;
        std::size_t minimum_elements_;
        std::vector<slot> slots_;
        std::uint64_t allocations_ = 0;
    };

    // The frame data in this frame's graph, imported outside any view. The passes read them as structured buffers
    // (the slot's SRV) and declare the reads on these handles.
    struct frame_buffers
    {
        graphics::buffer instances; // instance_data[]
        graphics::buffer instance_indices; // uint[], filled by the range walker (11.9)
        graphics::buffer lights; // light_data[]
        rhi::bindless_index instances_srv;
        rhi::bindless_index instance_indices_srv;
        rhi::bindless_index lights_srv;
        std::uint32_t instance_count = 0;
        std::uint32_t light_count = 0;
    };

    // The geometry arena in this frame's graph (11 §3.2), imported outside any view with the newest ready_after of the
    // mesh versions the frame draws (D-103). The scene passes read them (11.10).
    struct frame_geometry
    {
        graphics::buffer vertices;
        graphics::buffer indices;
    };

    // The material table in this frame's graph (11 §3.3), imported outside any view with the newest ready_after of the
    // slots the frame draws and of their textures. The textures themselves are read bindlessly, in `common`.
    struct frame_materials
    {
        graphics::buffer table;
    };

    struct frame_data_statistics
    {
        std::uint64_t uploads = 0; // frames that wrote an instance slot
        std::uint64_t reuses = 0; // frames that read the previous frame's instance slot (11 §3.5)
        std::uint64_t uploaded_bytes = 0; // copied by pass 0 since creation
        std::uint32_t instance_slot = 0; // read by the last frame
        std::uint32_t instance_slots = 0;
        std::uint64_t buffer_allocations = 0; // of the three rings, first creations included
        std::size_t staging_bytes = 0; // of every frame context's staging buffer
    };

    class frame_data_upload
    {
    public:
        // One staging buffer and one slot per frame in flight
        frame_data_upload(rhi::resources resources, std::uint32_t frames_in_flight);
        ~frame_data_upload();

        frame_data_upload(const frame_data_upload&) = delete;
        frame_data_upload& operator=(const frame_data_upload&) = delete;

        // Pass 0: imports the frame's slots into the graph and adds the copy pass that fills them. With `reuse`, the
        // instances and lights are the previous call's slots (the arrays given are ignored) and only the instance
        // indices are copied. `completed_frame_index` is the newest frame known complete. Fails with the RHI's error
        // when a buffer can't be created; nothing is added to the graph then.
        result<frame_buffers> add_upload_pass(
            graphics::render_graph& graph,
            const graphics::frame_token& frame,
            std::uint64_t completed_frame_index,
            std::span<const gpu::instance_data> instances,
            std::span<const gpu::light_data> lights,
            std::span<const std::uint32_t> instance_indices,
            bool reuse);

        // Whether add_upload_pass can reuse: a previous call wrote the instance and light slots
        bool can_reuse() const noexcept
        {
            return written_.has_value();
        }

        // Drops the slots a later reuse would read (the next frame writes new ones)
        void forget() noexcept
        {
            written_.reset();
        }

        frame_data_statistics statistics() const;

    private:
        struct staging
        {
            rhi::buffer_handle buffer;
            std::size_t capacity = 0; // bytes
        };

        // The slots of the last instance and light write, for reuse
        struct written_slots
        {
            std::uint32_t instances = 0;
            std::uint32_t lights = 0;
            std::uint32_t instance_count = 0;
            std::uint32_t light_count = 0;
        };

        result<std::span<std::byte>> staging_for(std::uint32_t context, std::size_t bytes);

        rhi::resources resources_;
        frame_buffer_ring instances_;
        frame_buffer_ring instance_indices_;
        frame_buffer_ring lights_;
        std::vector<staging> staging_; // per frame context
        std::optional<written_slots> written_;
        std::uint32_t last_instance_slot_ = 0;
        std::uint64_t uploads_ = 0;
        std::uint64_t reuses_ = 0;
        std::uint64_t uploaded_bytes_ = 0;
    };
} // namespace avis::visualizer::rendering

#endif
