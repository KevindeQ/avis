#ifndef AVIS_GRAPHICS_FRAME_TOKEN_H
#define AVIS_GRAPHICS_FRAME_TOKEN_H

// The token begin_frame delivers (docs/design/17-graphics-frames-and-transfers.md §4). Its own header because
// detail/frame_sender.h completes with it and frame_scheduler.h returns that sender; include frame_scheduler.h.

#include "avis/rhi/queue.h"

#include <cstdint>
#include <span>

namespace avis::graphics
{
    class frame_scheduler;

    // Frame N and its context N mod F. Move-only. Ended exactly once: explicitly with frame_scheduler::end_frame, or by
    // its destructor or a move assignment over it, with no submissions (17 §4, Q2). A moved-from or ended token holds
    // no frame and ends nothing. Must not outlive its scheduler.
    class frame_token
    {
    public:
        frame_token(frame_token&& other) noexcept;
        // Ends the frame this token holds first, like the destructor
        frame_token& operator=(frame_token&& other) noexcept;
        frame_token(const frame_token&) = delete;
        frame_token& operator=(const frame_token&) = delete;
        // Not ended: end_frame(*this, {}), logged at debug level
        ~frame_token();

        // N; the first frame is 1
        std::uint64_t index() const noexcept
        {
            return index_;
        }

        // N mod F
        std::uint32_t context_index() const noexcept
        {
            return context_index_;
        }

        // The newest frame known complete on the GPU when the token was delivered; at least N − F
        std::uint64_t completed_index() const noexcept
        {
            return completed_index_;
        }

        // Last submissions of frame N − F, which used this context before; all complete. Empty for the first F frames.
        // Valid until the token is ended (empty afterwards).
        std::span<const rhi::synchronization_point> recycled_submissions() const noexcept;

    private:
        friend class frame_scheduler;

        frame_token(
            frame_scheduler& owner,
            std::uint64_t index,
            std::uint32_t context_index,
            std::uint64_t completed_index) noexcept;

        // Ends an un-ended frame with no submissions and logs it
        void end_abandoned() noexcept;

        frame_scheduler* owner_ = nullptr; // null once ended or moved from
        std::uint64_t index_ = 0;
        std::uint32_t context_index_ = 0;
        std::uint64_t completed_index_ = 0;
    };
} // namespace avis::graphics

#endif
