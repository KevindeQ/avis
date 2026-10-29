#include "avis/rhi/d3d12/device_queues.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/conversions/queues.h"
#include "avis/rhi/d3d12/device.h"

#include <format>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        constexpr std::string_view queue_name(const queue_type type) noexcept
        {
            switch (type)
            {
            case queue_type::graphics:
                return "graphics";
            case queue_type::compute:
                return "compute";
            case queue_type::copy:
                return "copy";
            }
            return "unknown";
        }

        constexpr std::size_t index(const queue_type type) noexcept
        {
            return static_cast<std::size_t>(type);
        }

        constexpr std::uint64_t removed_fence_value = std::numeric_limits<std::uint64_t>::max();
    } // namespace

    device_queue::device_queue(device& device, device_queues& siblings, const queue_type type) noexcept :
        device_{ &device },
        siblings_{ &siblings },
        type_{ type }
    {}

    result<std::unique_ptr<device_queue>>
        device_queue::create(device& device, device_queues& siblings, const queue_type type)
    {
        std::unique_ptr<device_queue> queue{ new device_queue{ device, siblings, type } };
        const std::string_view name = queue_name(type);

        const D3D12_COMMAND_QUEUE_DESC description{ .Type = to_native(type),
                                                    .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
                                                    .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
                                                    .NodeMask = 0 };
        if (auto created = check(
                device.native().CreateCommandQueue(&description, IID_PPV_ARGS(&queue->queue_)),
                std::format("CreateCommandQueue ({})", name));
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        if (auto created = check(
                device.native().CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue->fence_)),
                std::format("CreateFence ({} queue)", name));
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }

        set_debug_name(device.debug(), *queue->queue_.get(), std::format("{} queue", name));
        set_debug_name(device.debug(), *queue->fence_.get(), std::format("{} queue fence", name));

        // Fails on a copy queue without timestamp support; 0 then
        UINT64 frequency = 0;
        if (SUCCEEDED(queue->queue_->GetTimestampFrequency(&frequency)))
        {
            queue->timestamp_frequency_ = frequency;
        }
        return queue;
    }

    result<std::uint64_t> device_queue::execute(
        const std::span<ID3D12CommandList* const> lists, const std::span<const synchronization_point> waits)
    {
        // The highest value per other queue that isn't complete yet
        std::array<std::uint64_t, 3> wait_values{};
        for (const synchronization_point point : waits)
        {
            if (point.queue == type_ || point.value == 0)
            {
                continue; // a queue's own submissions run in order
            }
            std::uint64_t& value = wait_values[index(point.queue)];
            if (point.value > value)
            {
                value = point.value;
            }
        }

        const std::lock_guard lock{ submission_mutex_ };
        if (device_->lost())
        {
            device_->report_removed("execute");
        }

        for (std::size_t other = 0; other < wait_values.size(); ++other)
        {
            const std::uint64_t value = wait_values[other];
            device_queue& producer = (*siblings_)[static_cast<queue_type>(other)];
            if (value == 0 || producer.is_complete(value))
            {
                continue;
            }
            if (auto waited =
                    check_device(*device_, queue_->Wait(&producer.fence(), value), "ID3D12CommandQueue::Wait");
                !waited)
            {
                return std::unexpected(std::move(waited.error()));
            }
        }

        if (!lists.empty())
        {
            queue_->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
        }

        return signal_timeline();
    }

    result<std::uint64_t> device_queue::signal_timeline()
    {
        const std::uint64_t value = next_value_;
        if (auto signalled = check_device(*device_, queue_->Signal(fence_.get(), value), "ID3D12CommandQueue::Signal");
            !signalled)
        {
            return std::unexpected(std::move(signalled.error()));
        }
        ++next_value_;
        last_submitted_.store(value, std::memory_order_release);
        return value;
    }

    // Both also advance the queue's own timeline, so last_submitted covers the shared fence operation: a shared fence
    // destroyed right after it is tagged behind it (§23.8)
    result<void> device_queue::signal(ID3D12Fence& fence, const std::uint64_t value)
    {
        const std::lock_guard lock{ submission_mutex_ };
        if (device_->lost())
        {
            device_->report_removed("signal");
        }
        if (auto signalled =
                check_device(*device_, queue_->Signal(&fence, value), "ID3D12CommandQueue::Signal (shared fence)");
            !signalled)
        {
            return signalled;
        }
        if (auto advanced = signal_timeline(); !advanced)
        {
            return std::unexpected(std::move(advanced.error()));
        }
        return {};
    }

    result<void> device_queue::wait(ID3D12Fence& fence, const std::uint64_t value)
    {
        const std::lock_guard lock{ submission_mutex_ };
        if (device_->lost())
        {
            device_->report_removed("wait");
        }
        if (auto waited = check_device(*device_, queue_->Wait(&fence, value), "ID3D12CommandQueue::Wait (shared fence)");
            !waited)
        {
            return waited;
        }
        if (auto advanced = signal_timeline(); !advanced)
        {
            return std::unexpected(std::move(advanced.error()));
        }
        return {};
    }

    std::uint64_t device_queue::last_submitted() const noexcept
    {
        return last_submitted_.load(std::memory_order_acquire);
    }

    std::uint64_t device_queue::completed() const noexcept
    {
        const std::uint64_t value = fence_->GetCompletedValue();
        if (value == removed_fence_value)
        {
            device_->mark_removed("ID3D12Fence::GetCompletedValue");
            return completed_cache_.load(std::memory_order_acquire);
        }

        std::uint64_t cached = completed_cache_.load(std::memory_order_relaxed);
        while (cached < value && !completed_cache_.compare_exchange_weak(
                                     cached, value, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
        }
        return cached < value ? value : cached;
    }

    bool device_queue::is_complete(const std::uint64_t value) const noexcept
    {
        return value == 0 || value <= completed_cache_.load(std::memory_order_acquire) || value <= completed();
    }

    queue_type device_queue::type() const noexcept
    {
        return type_;
    }

    std::uint64_t device_queue::timestamp_frequency() const noexcept
    {
        return timestamp_frequency_;
    }

    ID3D12CommandQueue& device_queue::native() noexcept
    {
        return *queue_.get();
    }

    ID3D12Fence& device_queue::fence() noexcept
    {
        return *fence_.get();
    }

    bool device_queue::enter_submit() noexcept
    {
        return !submitting_.exchange(true, std::memory_order_acquire);
    }

    void device_queue::leave_submit() noexcept
    {
        submitting_.store(false, std::memory_order_release);
    }

    result<std::unique_ptr<device_queues>> device_queues::create(device& device)
    {
        std::unique_ptr<device_queues> queues{ new device_queues };
        for (const queue_type type : { queue_type::graphics, queue_type::compute, queue_type::copy })
        {
            auto queue = device_queue::create(device, *queues, type);
            if (!queue)
            {
                return std::unexpected(std::move(queue.error()));
            }
            queues->queues_[index(type)] = std::move(*queue);
        }
        return queues;
    }

    device_queue& device_queues::operator[](const queue_type type) noexcept
    {
        return *queues_[index(type)];
    }

    const device_queue& device_queues::operator[](const queue_type type) const noexcept
    {
        return *queues_[index(type)];
    }

    bool device_queues::is_complete(const synchronization_point point) const noexcept
    {
        return (*this)[point.queue].is_complete(point.value);
    }

    release_tag device_queues::last_submitted() const noexcept
    {
        return { queues_[0]->last_submitted(), queues_[1]->last_submitted(), queues_[2]->last_submitted() };
    }

    release_tag device_queues::completed() const noexcept
    {
        return { queues_[0]->completed(), queues_[1]->completed(), queues_[2]->completed() };
    }
} // namespace avis::rhi::d3d12
