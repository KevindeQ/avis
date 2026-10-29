#ifndef AVIS_RHI_D3D12_DEVICE_QUEUES_H
#define AVIS_RHI_D3D12_DEVICE_QUEUES_H

// The three command queues, each with its timeline fence (docs/design/09-rhi.md §23.2)

#include "avis/error.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/completion_source.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/leaf_mutex.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace avis::rhi::d3d12
{
    class device;
    class device_queues;

    // One D3D12 command queue with its timeline fence. Not movable: the tracker keeps pointers to its fence.
    class device_queue
    {
    public:
        static result<std::unique_ptr<device_queue>> create(device& device, device_queues& siblings, queue_type type);

        device_queue(const device_queue&) = delete;
        device_queue& operator=(const device_queue&) = delete;

        // Waits on the other queues' fences (values already complete and waits on this queue are dropped, the
        // highest value per queue is kept), executes the lists, then signals the next value and returns it. An
        // empty `lists` is a bare Signal. Serialised by submission_mutex_. Throws device_lost after device removal.
        result<std::uint64_t>
            execute(std::span<ID3D12CommandList* const> lists, std::span<const synchronization_point> waits = {});

        // Shared fences (§23.9). Each also signals the queue's next timeline value, so last_submitted covers it. Throw
        // device_lost after device removal.
        result<void> signal(ID3D12Fence& fence, std::uint64_t value);
        result<void> wait(ID3D12Fence& fence, std::uint64_t value);

        // The value of the latest execute; 0 before the first
        std::uint64_t last_submitted() const noexcept;

        // GetCompletedValue, cached and monotonic. UINT64_MAX (the removal signal) marks the device lost and returns
        // the cached value instead.
        std::uint64_t completed() const noexcept;

        // value 0, or at most the cached completed value, without a D3D12 call; otherwise refreshes the cache once
        bool is_complete(std::uint64_t value) const noexcept;

        queue_type type() const noexcept;

        // Ticks per second (GetTimestampFrequency at creation); 0 when the queue has no timestamps
        std::uint64_t timestamp_frequency() const noexcept;

        ID3D12CommandQueue& native() noexcept;
        ID3D12Fence& fence() noexcept;

        // queue<Queue>::submit isn't thread-safe against itself (§5): true when no other submit is in progress. Debug
        // builds assert it; leave_submit ends the section.
        bool enter_submit() noexcept;
        void leave_submit() noexcept;

    private:
        device_queue(device& device, device_queues& siblings, queue_type type) noexcept;

        // Signal(fence_, next_value_++) and publish it; under submission_mutex_
        result<std::uint64_t> signal_timeline();

        device* device_;
        device_queues* siblings_; // other queues' fences for cross-queue waits
        queue_type type_;
        com_ptr<ID3D12CommandQueue> queue_;
        com_ptr<ID3D12Fence> fence_; // timeline, starts at 0
        std::uint64_t timestamp_frequency_ = 0;
        leaf_mutex submission_mutex_{ "device_queue::submission_mutex_" };
        std::uint64_t next_value_ = 1; // guarded by submission_mutex_
        std::atomic<std::uint64_t> last_submitted_{ 0 };
        mutable std::atomic<std::uint64_t> completed_cache_{ 0 };
        std::atomic<bool> submitting_{ false };
    };

    // The graphics, compute and copy queues
    class device_queues : public completion_source
    {
    public:
        static result<std::unique_ptr<device_queues>> create(device& device);

        device_queues(const device_queues&) = delete;
        device_queues& operator=(const device_queues&) = delete;

        device_queue& operator[](queue_type type) noexcept;
        const device_queue& operator[](queue_type type) const noexcept;

        bool is_complete(synchronization_point point) const noexcept override;

        // Per queue, indexed by queue_type (§23.8)
        release_tag last_submitted() const noexcept;
        release_tag completed() const noexcept;

    private:
        device_queues() noexcept = default;

        std::array<std::unique_ptr<device_queue>, 3> queues_;
    };
} // namespace avis::rhi::d3d12

#endif
