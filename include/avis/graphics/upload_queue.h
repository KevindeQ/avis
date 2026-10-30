#ifndef AVIS_GRAPHICS_UPLOAD_QUEUE_H
#define AVIS_GRAPHICS_UPLOAD_QUEUE_H

// CPU→GPU uploads through a staging ring, flushed on the copy queue (docs/design/17-graphics-frames-and-transfers.md
// §7, D-100, D-103). Several queues per RHI are independent objects. Free of <windows.h> and D3D12.

#include "avis/error.h"
#include "avis/graphics/detail/upload_sender.h"
#include "avis/graphics/upload_ticket.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace avis::graphics
{
    namespace detail
    {
        struct upload_queue_state; // lib/avis/graphics/upload_queue.cpp
        struct staging_block;
        struct space_wait_receiver;
    } // namespace detail

    struct upload_queue_configuration
    {
        std::size_t staging_ring_size = std::size_t{ 64 } << 20; // rounded up to a multiple of 64 KiB
    };

    class upload_queue
    {
    public:
        // Creates the ring: one upload-heap buffer, mapped for the queue's lifetime. Fails with invalid_argument for a
        // ring size of 0, and passes the RHI's errors through.
        // Views (D-96): ring and dedicated buffers, the flush's copy list and submission, waits for space
        static result<std::unique_ptr<upload_queue>> create(
            rhi::resources resources,
            rhi::command_lists command_lists,
            rhi::queue<rhi::queue_type::copy> copy_queue,
            rhi::synchronization synchronization,
            const upload_queue_configuration& configuration);

        // No async upload may be outstanding; destroy before the RHI. Queued, unflushed copies are dropped.
        ~upload_queue();

        upload_queue(const upload_queue&) = delete;
        upload_queue& operator=(const upload_queue&) = delete;

        // Copy into the ring and queue the copy for the next flush; fail with upload_queue_full instead of waiting
        // (A-59). Requests larger than half the ring use a dedicated upload buffer and never fail for ring space.
        // invalid_argument for a destination that isn't a gpu_only buffer, or a range outside it. Any thread.
        result<upload_ticket> upload_buffer(
            rhi::buffer_handle destination, std::uint64_t destination_offset, std::span<const std::byte> data);

        // One copy per subresource, laid out with rhi::resources::copyable_footprint. The texture is in `common`
        // afterwards. invalid_argument for a subresource outside the texture or data shorter than its rows. Any thread.
        result<upload_ticket> upload_texture(rhi::texture_handle destination, std::span<const subresource_data> data);

        // Like upload_buffer and upload_texture, but wait for ring space instead of failing with upload_queue_full:
        // complete once the data is in the ring, inline when it fits now, otherwise from poll_completions() once an
        // earlier flush completed. Waiters are served in FIFO order. Other errors complete inline. The data (and the
        // subresource span) must stay valid until completion.
        upload_sender upload_buffer_async(
            rhi::buffer_handle destination, std::uint64_t destination_offset, std::span<const std::byte> data) noexcept;
        upload_sender
            upload_texture_async(rhi::texture_handle destination, std::span<const subresource_data> data) noexcept;

        // Records the queued copies on a copy list and submits it on the copy queue. Returns the submission's point,
        // or the last flush's point when nothing was queued. An acquire or submit failure keeps the copies queued.
        result<rhi::synchronization_point> flush();

        // The copy-queue point a ticket waits on: the point of its flush, a default point (always complete) once that
        // flush is complete and retired, or nullopt before its flush. A ticket of another queue asserts.
        std::optional<rhi::synchronization_point> point(upload_ticket ticket) const noexcept;

    private:
        friend class detail::upload_waiter;
        friend struct detail::space_wait_receiver;

        upload_queue(
            rhi::resources resources,
            rhi::command_lists command_lists,
            rhi::queue<rhi::queue_type::copy> copy_queue,
            rhi::synchronization synchronization,
            std::unique_ptr<detail::upload_queue_state> state) noexcept;

        // Ring space (or a dedicated buffer) for `size` bytes; upload_queue_full when the ring has no room
        result<detail::staging_block> stage(std::uint64_t size);
        void retire_locked();

        // ---- async uploads (17 §7.2) ----
        result<upload_ticket> upload(const detail::upload_request& request);
        // The ring bytes a request that failed with upload_queue_full needs
        std::uint64_t staging_size(const detail::upload_request& request) const;
        // Whether `size` more bytes could be reserved now, after the waiters before it (under the mutex)
        bool fits_locked(std::uint64_t size, std::uint64_t& simulated_head) const noexcept;
        // Waits for an in-flight flush, then resumes the waiters that fit; started by whoever set space_wait_running
        void start_space_wait(rhi::synchronization_point point) noexcept;
        void space_available() noexcept;
        void space_wait_failed(avis::error error) noexcept;
        void space_wait_stopped() noexcept;

        rhi::resources resources_;
        rhi::command_lists command_lists_;
        rhi::queue<rhi::queue_type::copy> copy_queue_;
        rhi::synchronization synchronization_;
        std::unique_ptr<detail::upload_queue_state> state_;
    };
} // namespace avis::graphics

#endif
