#ifndef AVIS_RHI_D3D12_COMPLETION_TRACKER_H
#define AVIS_RHI_D3D12_COMPLETION_TRACKER_H

// Waiters per timeline and the completion event (docs/design/09-rhi.md §23.3, §23.4). The RHI owns no thread: a
// higher layer calls poll_completions(), usually when the event fires.

#include "avis/error.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/leaf_mutex.h"
#include "avis/rhi/d3d12/unique_handle.h"
#include "avis/rhi/detail/completion_waiter.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace avis::rhi::d3d12
{
    class device;
    class device_queues;

    // A fence whose values waiters wait for: a queue's timeline fence, or a shared fence while it has waiters
    struct timeline
    {
        static constexpr std::uint8_t shared_index = 3;

        ID3D12Fence* fence = nullptr;
        std::uint8_t index = 0; // the waiters' queue_values index, or shared_index for a shared fence
        std::uint64_t armed_value = 0; // the value the completion event is registered for; 0 = none
        detail::completion_waiter* front = nullptr; // sorted by the value waited for on this timeline
        detail::completion_waiter* back = nullptr;
    };

    class completion_tracker
    {
    public:
        enum class add_outcome : std::uint8_t
        {
            added, // queued: completes from process(), or from remove() on a stop request
            reached, // every value already reached: the caller completes inline
            stopped, // remove() came first: the caller completes with set_stopped
            failed // device lost: the caller completes with set_error
        };

        // Creates the auto-reset completion event
        static result<std::unique_ptr<completion_tracker>> create(device& device, device_queues& queues);

        // Completes leftover waiters with on_stopped (logged) and closes the event
        ~completion_tracker();

        completion_tracker(const completion_tracker&) = delete;
        completion_tracker& operator=(const completion_tracker&) = delete;

        // Queues the waiter on its first unreached timeline. Checked under the mutex, so a value reached in between
        // isn't missed.
        add_outcome add(detail::completion_waiter& waiter) noexcept;

        // From a stop callback, any thread. True when the waiter was queued: the caller completes it with set_stopped.
        // False when process() already took it (its completion is on the way), or when it wasn't added yet (it's
        // marked cancelled, and add() reports stopped).
        bool remove(detail::completion_waiter& waiter) noexcept;

        // completion_driver::poll_completions() through device_state. Non-blocking, any thread, concurrent calls
        // don't wait for each other. Returns the number of waiters completed (reached or failed).
        std::size_t process() noexcept;

        HANDLE event() const noexcept;

        // SetEvent: the driver calls poll_completions() again soon (deferred releases left after the cap, §23.8)
        void signal() noexcept;

        bool has_waiters() const noexcept;
        std::chrono::steady_clock::time_point last_processed() const noexcept;

    private:
        completion_tracker(device& device, device_queues& queues) noexcept;

        static std::uint64_t value_on(const detail::completion_waiter& waiter, std::size_t line) noexcept;

        // The timeline the waiter is queued on: a queue's, or its shared fence's
        timeline& line_of(detail::completion_waiter& waiter) noexcept;

        // A shared fence's first waiter attaches its timeline, so process() polls it; its last one detaches it (§23.9)
        void attach(timeline& line) noexcept;
        void detach(timeline& line) noexcept;

        // Sorted insert; true when the waiter became the front
        static bool insert(timeline& line, detail::completion_waiter& waiter) noexcept;
        static void unlink(timeline& line, detail::completion_waiter& waiter) noexcept;

        // Queues the waiter on its first unreached timeline at or after `first`; false when every value is reached
        bool enqueue(detail::completion_waiter& waiter, std::size_t first) noexcept;

        // SetEventOnCompletion(front value) when it differs from the armed value
        void arm(timeline& line) noexcept;

        // Debug builds: true when waiters have been pending for over a second, process() hasn't run for a second
        // and the last warning is a second old. Under the mutex.
        bool watchdog_due(std::chrono::steady_clock::time_point now) noexcept;

        device& device_;
        device_queues& queues_;
        unique_handle event_;
        leaf_mutex mutex_{ "completion_tracker::mutex_" };
        std::array<timeline, 3> timelines_; // indexed by queue_type
        std::vector<timeline*> shared_; // shared fences with waiters
        std::atomic<std::size_t> waiter_count_{ 0 };
        std::atomic<bool> again_{ false }; // set when process() found another caller running
        std::atomic<std::chrono::steady_clock::rep> last_processed_;
        std::chrono::steady_clock::time_point pending_since_; // when the tracker went from no waiters to some
        std::chrono::steady_clock::time_point last_warning_;
    };
} // namespace avis::rhi::d3d12

#endif
