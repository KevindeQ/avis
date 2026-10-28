#include "avis/rhi/d3d12/completion_tracker.h"

#include "avis/log.h"
#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/error.h"

#include <mutex>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        using clock = std::chrono::steady_clock;
        using waiter = detail::completion_waiter;

        constexpr auto watchdog_interval = std::chrono::seconds{ 1 };

        // A singly linked list through next_, for waiters taken out of the timelines
        struct taken_list
        {
            waiter* front = nullptr;
            waiter* back = nullptr;
            std::size_t size = 0;
        };
    } // namespace

    completion_tracker::completion_tracker(device& device, device_queues& queues) noexcept :
        device_{ device },
        queues_{ queues },
        last_processed_{ clock::now().time_since_epoch().count() },
        pending_since_{ clock::now() }
    {}

    result<std::unique_ptr<completion_tracker>> completion_tracker::create(device& device, device_queues& queues)
    {
        std::unique_ptr<completion_tracker> tracker{ new completion_tracker{ device, queues } };

        tracker->event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!tracker->event_)
        {
            return std::unexpected(make_error(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent (completion event)"));
        }

        for (const queue_type type : { queue_type::graphics, queue_type::compute, queue_type::copy })
        {
            timeline& line = tracker->timelines_[static_cast<std::size_t>(type)];
            line.fence = &queues[type].fence();
            line.index = static_cast<std::uint8_t>(type);
        }
        return tracker;
    }

    completion_tracker::~completion_tracker()
    {
        // Single-threaded now: no sender may still be starting or stopping (§4.2)
        std::size_t leftover = 0;
        for (timeline& line : timelines_)
        {
            while (waiter* const pending = line.front)
            {
                unlink(line, *pending);
                pending->state_ = waiter::waiter_state::taken;
                ++leftover;
                pending->on_stopped();
            }
        }
        if (leftover > 0)
        {
            try
            {
                log::get("rhi")->warn(
                    "completion_tracker: {} GPU wait(s) still pending when the RHI was destroyed, completed with "
                    "set_stopped",
                    leftover);
            }
            catch (...)
            {}
        }
    }

    std::uint64_t completion_tracker::value_on(const waiter& waiter, const std::size_t line) noexcept
    {
        return waiter.queue_values[line];
    }

    bool completion_tracker::insert(timeline& line, waiter& waiter) noexcept
    {
        const std::uint64_t value = value_on(waiter, line.index);

        // New waits usually wait for the latest values, so search from the back
        detail::completion_waiter* after = line.back;
        while (after != nullptr && value_on(*after, line.index) > value)
        {
            after = after->previous_;
        }

        waiter.previous_ = after;
        waiter.next_ = after != nullptr ? after->next_ : line.front;
        if (waiter.next_ != nullptr)
        {
            waiter.next_->previous_ = &waiter;
        }
        else
        {
            line.back = &waiter;
        }
        if (after != nullptr)
        {
            after->next_ = &waiter;
        }
        else
        {
            line.front = &waiter;
        }
        waiter.current_timeline_ = line.index;
        waiter.state_ = waiter::waiter_state::queued;
        return line.front == &waiter;
    }

    void completion_tracker::unlink(timeline& line, waiter& waiter) noexcept
    {
        if (waiter.previous_ != nullptr)
        {
            waiter.previous_->next_ = waiter.next_;
        }
        else
        {
            line.front = waiter.next_;
        }
        if (waiter.next_ != nullptr)
        {
            waiter.next_->previous_ = waiter.previous_;
        }
        else
        {
            line.back = waiter.previous_;
        }
        waiter.previous_ = nullptr;
        waiter.next_ = nullptr;
    }

    bool completion_tracker::enqueue(waiter& waiter, const std::size_t first) noexcept
    {
        for (std::size_t index = first; index < timelines_.size(); ++index)
        {
            const std::uint64_t value = value_on(waiter, index);
            if (value == 0 || queues_[static_cast<queue_type>(index)].is_complete(value))
            {
                continue;
            }
            timeline& line = timelines_[index];
            if (insert(line, waiter))
            {
                arm(line);
            }
            return true;
        }
        return false;
    }

    void completion_tracker::arm(timeline& line) noexcept
    {
        if (line.front == nullptr)
        {
            return; // a stale registration may still fire; process() then finds nothing due
        }
        const std::uint64_t value = value_on(*line.front, line.index);
        if (value == line.armed_value)
        {
            return; // that registration is still pending: D3D12 keeps it until the value is reached
        }

        // When the value is already reached, D3D12 signals the event at once
        const HRESULT result = line.fence->SetEventOnCompletion(value, event_.get());
        if (SUCCEEDED(result))
        {
            line.armed_value = value;
            return;
        }
        if (is_device_removed_code(result))
        {
            device_.mark_removed("ID3D12Fence::SetEventOnCompletion");
        }
        else
        {
            try
            {
                log::get("rhi")->error(
                    "completion_tracker: {}", make_error(result, "ID3D12Fence::SetEventOnCompletion"));
            }
            catch (...)
            {}
        }
        // Wake the driver, so process() fails the waiters (device lost) or retries arming
        SetEvent(event_.get());
    }

    bool completion_tracker::watchdog_due([[maybe_unused]] const clock::time_point now) noexcept
    {
#ifdef NDEBUG
        return false;
#else
        if (now - pending_since_ <= watchdog_interval || now - last_processed() <= watchdog_interval ||
            now - last_warning_ <= watchdog_interval)
        {
            return false;
        }
        last_warning_ = now;
        return true;
#endif
    }

    completion_tracker::add_outcome completion_tracker::add(waiter& waiter) noexcept
    {
        bool warn = false;
        add_outcome outcome = add_outcome::added;
        {
            const std::lock_guard lock{ mutex_ };
            if (waiter.state_ == waiter::waiter_state::cancelled)
            {
                waiter.state_ = waiter::waiter_state::taken;
                return add_outcome::stopped;
            }
            if (device_.lost())
            {
                return add_outcome::failed;
            }

            const bool was_empty = waiter_count_.load(std::memory_order_relaxed) == 0;
            if (!enqueue(waiter, 0))
            {
                outcome = device_.lost() ? add_outcome::failed : add_outcome::reached;
            }
            else
            {
                waiter_count_.fetch_add(1, std::memory_order_relaxed);
                const clock::time_point now = clock::now();
                if (was_empty)
                {
                    pending_since_ = now;
                }
                warn = watchdog_due(now);
            }
        }

        if (warn)
        {
            try
            {
                log::get("rhi")->warn(
                    "completion_tracker: GPU waits have been pending for over a second and poll_completions() hasn't "
                    "run since. Nothing completes a GPU wait unless something drives the RHI (09-rhi.md §5.2).");
            }
            catch (...)
            {}
        }
        return outcome;
    }

    bool completion_tracker::remove(waiter& waiter) noexcept
    {
        const std::lock_guard lock{ mutex_ };
        switch (waiter.state_)
        {
        case waiter::waiter_state::queued:
            {
                timeline& line = timelines_[waiter.current_timeline_];
                const bool was_front = line.front == &waiter;
                unlink(line, waiter);
                waiter.state_ = waiter::waiter_state::taken;
                waiter_count_.fetch_sub(1, std::memory_order_relaxed);
                if (was_front)
                {
                    arm(line);
                }
                return true;
            }
        case waiter::waiter_state::idle:
            waiter.state_ = waiter::waiter_state::cancelled;
            return false;
        case waiter::waiter_state::taken:
        case waiter::waiter_state::cancelled:
            return false;
        }
        return false;
    }

    std::size_t completion_tracker::process() noexcept
    {
        std::size_t completed = 0;
        for (;;)
        {
            // §23.4 step 1: a caller that finds the lock taken asks the holder to run again, then tries once more
            if (!mutex_.try_lock())
            {
                again_.store(true, std::memory_order_release);
                if (!mutex_.try_lock())
                {
                    return completed;
                }
            }

            taken_list reached;
            taken_list failed;
            const auto take = [](taken_list& list, waiter& taken)
            {
                taken.state_ = waiter::waiter_state::taken;
                taken.next_ = nullptr;
                if (list.back != nullptr)
                {
                    list.back->next_ = &taken;
                }
                else
                {
                    list.front = &taken;
                }
                list.back = &taken;
                ++list.size;
            };
            const auto fail_all = [&]
            {
                for (timeline& line : timelines_)
                {
                    while (waiter* const pending = line.front)
                    {
                        unlink(line, *pending);
                        take(failed, *pending);
                    }
                }
            };

            if (device_.lost())
            {
                fail_all();
            }
            else
            {
                for (timeline& line : timelines_)
                {
                    if (line.front == nullptr)
                    {
                        continue;
                    }
                    // One GetCompletedValue per timeline with waiters
                    const std::uint64_t done = queues_[static_cast<queue_type>(line.index)].completed();
                    if (device_.lost())
                    {
                        fail_all();
                        break;
                    }

                    bool front_changed = false;
                    while (line.front != nullptr && value_on(*line.front, line.index) <= done)
                    {
                        waiter& due = *line.front;
                        unlink(line, due);
                        front_changed = true;
                        // Move on to its next unreached timeline (a later one, so this loop still visits it)
                        if (!enqueue(due, line.index + std::size_t{ 1 }))
                        {
                            take(reached, due);
                        }
                    }
                    if (front_changed)
                    {
                        arm(line);
                    }
                }
            }

            waiter_count_.fetch_sub(reached.size + failed.size, std::memory_order_relaxed);
            last_processed_.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
            mutex_.unlock();

            // Outside the lock: a completion may start a new wait, submit or destroy objects
            for (waiter* due = reached.front; due != nullptr;)
            {
                waiter* const next = due->next_; // the completion may destroy the waiter
                due->on_reached();
                due = next;
            }
            for (waiter* due = failed.front; due != nullptr;)
            {
                waiter* const next = due->next_;
                due->on_failed(device_lost_error("GPU wait: the D3D12 device was removed"));
                due = next;
            }
            completed += reached.size + failed.size;

            // §23.4 step 2: run again when another caller asked while this one held the lock
            if (!again_.exchange(false, std::memory_order_acq_rel))
            {
                return completed;
            }
        }
    }

    HANDLE completion_tracker::event() const noexcept
    {
        return event_.get();
    }

    void completion_tracker::signal() noexcept
    {
        SetEvent(event_.get());
    }

    bool completion_tracker::has_waiters() const noexcept
    {
        return waiter_count_.load(std::memory_order_relaxed) > 0;
    }

    clock::time_point completion_tracker::last_processed() const noexcept
    {
        return clock::time_point{ clock::duration{ last_processed_.load(std::memory_order_relaxed) } };
    }
} // namespace avis::rhi::d3d12
