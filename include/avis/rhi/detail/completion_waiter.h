#ifndef AVIS_RHI_DETAIL_COMPLETION_WAITER_H
#define AVIS_RHI_DETAIL_COMPLETION_WAITER_H

// The intrusive node the completion tracker keeps per waiting operation (docs/design/09-rhi.md §23.4). Public only
// because the sender templates derive from it (§16); free of <windows.h> and D3D12.

#include "avis/error.h"

#include <array>
#include <cstdint>

namespace avis::rhi::d3d12
{
    class completion_tracker;
    struct shared_fence_state;
} // namespace avis::rhi::d3d12

namespace avis::rhi::detail
{
    class completion_waiter
    {
    public:
        // Every value reached. Called once, outside the tracker's lock.
        virtual void on_reached() noexcept = 0;
        // Device lost
        virtual void on_failed(avis::error error) noexcept = 0;
        // The tracker was destroyed with the waiter still queued (shutdown with a sender still running). A stop
        // request is completed by whoever removed the waiter instead (completion_tracker::remove).
        virtual void on_stopped() noexcept = 0;

    protected:
        completion_waiter() noexcept = default;
        ~completion_waiter() = default;
        completion_waiter(const completion_waiter&) = delete;
        completion_waiter& operator=(const completion_waiter&) = delete;

        // Any set of synchronization points reduces to at most one value per queue (the latest), indexed by
        // queue_type. 0 = nothing to wait for on that queue.
        std::array<std::uint64_t, 3> queue_values{};
        // Or one value of a shared fence (when_reached, §13); the queue values are all 0 then
        d3d12::shared_fence_state* shared_fence = nullptr;
        std::uint64_t shared_value = 0;

    private:
        friend class d3d12::completion_tracker;

        // Guarded by the tracker's mutex
        enum class waiter_state : std::uint8_t
        {
            idle, // not added yet, or completed inline
            queued, // on a timeline
            taken, // removed by process() or by remove()
            cancelled // remove() came before add(): add() reports it stopped
        };

        completion_waiter* previous_ = nullptr;
        completion_waiter* next_ = nullptr;
        std::uint8_t current_timeline_ = 0; // 0–2: a queue; 3: shared_fence's timeline
        waiter_state state_ = waiter_state::idle;
    };
} // namespace avis::rhi::detail

#endif
