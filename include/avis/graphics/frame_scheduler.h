#ifndef AVIS_GRAPHICS_FRAME_SCHEDULER_H
#define AVIS_GRAPHICS_FRAME_SCHEDULER_H

// Frame pacing with F frames in flight and the per-frame services (docs/design/17-graphics-frames-and-transfers.md
// §3–5). Waits through rhi::synchronization and never drives the RHI: a frame that has to wait for the GPU completes
// from the application's completion_driver::poll_completions(). Free of <windows.h> and D3D12.

#include "avis/error.h"
#include "avis/graphics/detail/frame_sender.h"
#include "avis/graphics/frame_token.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/transient.h"
#include "avis/rhi/types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace avis::graphics
{
    namespace detail
    {
        struct frame_context; // lib/avis/graphics/frame_scheduler.cpp
    } // namespace detail

    struct frame_scheduler_configuration
    {
        std::uint32_t frames_in_flight = 2; // 1..3
        std::size_t upload_memory_per_frame = std::size_t{ 16 } << 20; // linear upload allocator per context
        std::uint32_t descriptors_per_frame = 32'768; // descriptor range per context; F × this must fit the RHI's
                                                      // transient_descriptor_count
    };

    class frame_scheduler
    {
    public:
        // Creates the per-context transient objects. Fails with invalid_argument for frames_in_flight outside 1..3 or
        // a size of 0, and passes the RHI's errors through (descriptor ranges that don't fit the transient region).
        // Views (D-96): waits for frame N − F; the per-context transient objects
        static result<std::unique_ptr<frame_scheduler>> create(
            rhi::synchronization synchronization,
            rhi::transient_objects transient_objects,
            const frame_scheduler_configuration& configuration);

        // No frame may be outstanding; destroy before the RHI
        ~frame_scheduler();

        frame_scheduler(const frame_scheduler&) = delete;
        frame_scheduler& operator=(const frame_scheduler&) = delete;

        // ---- pacing; serialised by the caller (one begin_frame outstanding, asserted) ----

        // Completes once the last submissions of frame N − F are complete and context N mod F is recycled: inline
        // when they already are, otherwise from poll_completions() on the thread that calls it
        frame_sender begin_frame() noexcept;

        // Never waits: std::nullopt when the context isn't free yet
        std::optional<frame_token> try_begin_frame();

        // Records, per queue, the latest of `last_submissions`. With no submissions the context is free at once; that
        // is only correct when nothing was submitted under the frame (17 §4).
        void end_frame(frame_token&& frame, std::span<const rhi::synchronization_point> last_submissions);

        std::uint32_t frames_in_flight() const noexcept;

        // ---- per-frame services: any thread, concurrently, for the frame being recorded; lock-free ----
        // They fail with the RHI's transient_memory_exhausted when the context is full, and log one warning per frame
        // with the budget to raise (D-98). A token that isn't the frame being recorded fails a debug check and
        // invalid_argument.

        // From the context's linear upload allocator: write-only, valid until the context is recycled
        result<rhi::upload_allocation>
            allocate_frame_memory(const frame_token& frame, std::size_t size, std::size_t alignment = 256);

        // Copies `value` into 256-byte aligned frame memory, for a root CBV
        template<class T>
            requires std::is_trivially_copyable_v<T>
        result<rhi::gpu_address> write_frame_constants(const frame_token& frame, const T& value)
        {
            auto allocation = allocate_frame_memory(frame, sizeof(T), 256);
            if (!allocation)
            {
                return std::unexpected(std::move(allocation.error()));
            }
            std::memcpy(allocation->cpu.data(), &value, sizeof(T));
            return allocation->gpu;
        }

        // Views in the context's descriptor range, for placed (transient) resources; valid until it is recycled
        result<rhi::bindless_index> create_frame_srv(
            const frame_token& frame,
            rhi::texture_handle texture,
            rhi::subresource_range range = rhi::all_subresources);
        result<rhi::bindless_index>
            create_frame_uav(const frame_token& frame, rhi::texture_handle texture, std::uint32_t mip = 0);
        result<rhi::bindless_index> create_frame_srv(const frame_token& frame, rhi::buffer_handle buffer);
        result<rhi::bindless_index> create_frame_uav(const frame_token& frame, rhi::buffer_handle buffer);

    private:
        friend class frame_token;
        friend class detail::frame_operation;

        enum class pacing : std::uint8_t
        {
            idle, // no frame outstanding: begin_frame may start
            waiting, // a begin_frame was started and hasn't completed
            recording // a token was delivered and isn't ended
        };

        frame_scheduler(
            rhi::synchronization synchronization,
            const frame_scheduler_configuration& configuration,
            std::vector<std::unique_ptr<detail::frame_context>> contexts) noexcept;

        // idle → waiting; a failed debug check (and false) when a frame is outstanding
        bool try_enter_waiting() noexcept;
        void leave_waiting() noexcept;
        detail::frame_context& next_context() noexcept;
        bool next_context_is_free() const;
        // Recycles the next context (§5) and makes its token; waiting → recording
        frame_token recycle_and_deliver();
        std::span<const rhi::synchronization_point> recycled_submissions(const frame_token& frame) const noexcept;

        // The context of the frame being recorded; a failed debug check and invalid_argument for any other token
        result<detail::frame_context*> recording_context(const frame_token& frame, const char* service) noexcept;
        // The once-per-frame exhaustion warnings
        void warn_memory_exhausted(detail::frame_context& context, const frame_token& frame) const noexcept;
        void warn_descriptors_exhausted(detail::frame_context& context, const frame_token& frame) const noexcept;
        result<rhi::bindless_index> checked_view(
            detail::frame_context& context, const frame_token& frame, result<rhi::bindless_index> view) const noexcept;

        rhi::synchronization synchronization_;
        frame_scheduler_configuration configuration_;
        std::vector<std::unique_ptr<detail::frame_context>> contexts_;
        std::atomic<pacing> pacing_{ pacing::idle };
        std::uint64_t delivered_index_ = 0; // the last frame delivered; frame N + 1 comes next
        std::uint64_t completed_index_ = 0; // published by the recycle (§5, step 1)
        std::atomic<std::uint64_t> current_index_{ 0 }; // the frame being recorded; 0 when none
    };
} // namespace avis::graphics

#endif
