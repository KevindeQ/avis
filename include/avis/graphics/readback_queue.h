#ifndef AVIS_GRAPHICS_READBACK_QUEUE_H
#define AVIS_GRAPHICS_READBACK_QUEUE_H

// GPU→CPU readbacks into pooled readback buffers, delivered as leases (docs/design/17-graphics-frames-and-transfers.md
// §8, D-99). For tests, tools and work outside the frame; in-frame readbacks go through the render graph. Free of
// <windows.h> and D3D12.

#include "avis/error.h"
#include "avis/graphics/detail/readback_sender.h"
#include "avis/graphics/readback_data.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace avis::graphics
{
    namespace detail
    {
        struct readback_queue_state; // lib/avis/graphics/readback_queue.cpp
        struct pooled_buffer;
    } // namespace detail

    // The pool's counters, for tests and diagnostics
    struct readback_pool_statistics
    {
        std::uint64_t buffers_created = 0; // pooled and exact-size
        std::uint64_t buffers_destroyed = 0; // trimmed, exact-size, and those of stopped or failed readbacks
        std::size_t pooled_buffers = 0; // free in the pool now
        std::size_t leases = 0; // delivered and not destroyed
        std::size_t readbacks = 0; // started and not completed
    };

    class readback_queue
    {
    public:
        // Smallest size class; classes are powers of two from here up to largest_pooled_size
        static constexpr std::uint64_t smallest_pooled_size = std::uint64_t{ 64 } << 10;
        // Larger reads get an exact-size buffer that isn't pooled
        static constexpr std::uint64_t largest_pooled_size = std::uint64_t{ 64 } << 20;
        // Free buffers kept per size class; a lease returning to a full class destroys its buffer
        static constexpr std::size_t buffers_per_class = 4;

        // Views (D-96): pooled buffers, copy lists and submission on the queue a readback names, completion waits.
        // Creates no buffer yet.
        static result<std::unique_ptr<readback_queue>> create(
            rhi::resources resources,
            rhi::command_lists command_lists,
            rhi::queue<rhi::queue_type::graphics> graphics_queue,
            rhi::queue<rhi::queue_type::compute> compute_queue,
            rhi::queue<rhi::queue_type::copy> copy_queue,
            rhi::synchronization synchronization);

        // No lease or readback may be outstanding; destroy before the RHI. Destroys the pooled buffers.
        ~readback_queue();

        readback_queue(const readback_queue&) = delete;
        readback_queue& operator=(const readback_queue&) = delete;

        // Record a copy of `size` bytes from `offset` into a pooled buffer on `queue`, submit it, and complete when its
        // point is reached. The source must be readable by the copy (copy_source, or common on the copy queue) and its
        // data already written: the copy waits for nothing. invalid_argument for a range outside the source or a size
        // of 0. Any thread.
        readback_buffer_sender read_buffer(
            rhi::buffer_handle source,
            std::uint64_t offset,
            std::uint64_t size,
            rhi::queue_type queue = rhi::queue_type::copy) noexcept;

        // The same for one texture subresource, laid out with rhi::resources::copyable_footprint. invalid_argument for
        // a subresource outside the texture.
        readback_texture_sender read_texture(
            rhi::texture_handle source,
            rhi::subresource source_subresource = {},
            rhi::queue_type queue = rhi::queue_type::graphics) noexcept;

        readback_pool_statistics statistics() const;

    private:
        friend class detail::readback_operation;
        friend class detail::readback_lease;

        readback_queue(
            rhi::resources resources,
            rhi::command_lists command_lists,
            rhi::queue<rhi::queue_type::graphics> graphics_queue,
            rhi::queue<rhi::queue_type::compute> compute_queue,
            rhi::queue<rhi::queue_type::copy> copy_queue,
            rhi::synchronization synchronization,
            std::unique_ptr<detail::readback_queue_state> state) noexcept;

        // A free buffer of the size class for `size` (or an exact-size one), created when the class is empty
        result<detail::pooled_buffer> take_buffer(std::uint64_t size);
        // A lease ended, or a copy was never submitted: back to the pool, or destroyed when the class is full or the
        // buffer isn't pooled
        void return_buffer(rhi::buffer_handle buffer, int size_class, bool ends_lease) noexcept;
        // A readback stopped or failed after its copy was submitted
        void discard_buffer(rhi::buffer_handle buffer) noexcept;

        // Records the request's copy into `destination` on the request's queue and submits it
        template<rhi::queue_type Queue>
        result<rhi::synchronization_point>
            record_and_submit(const detail::readback_request& request, rhi::buffer_handle destination);

        rhi::resources resources_;
        rhi::command_lists command_lists_;
        rhi::queue<rhi::queue_type::graphics> graphics_queue_;
        rhi::queue<rhi::queue_type::compute> compute_queue_;
        rhi::queue<rhi::queue_type::copy> copy_queue_;
        rhi::synchronization synchronization_;
        std::unique_ptr<detail::readback_queue_state> state_;
    };
} // namespace avis::graphics

#endif
