#include "avis/rhi/d3d12/lifetime_manager.h"

#include "avis/log.h"
#include "avis/rhi/d3d12/device_queues.h"

#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        using clock = std::chrono::steady_clock;

        [[maybe_unused]] constexpr auto open_list_warning_interval = std::chrono::seconds{ 1 };
    } // namespace

    lifetime_manager::lifetime_manager(device_queues& queues) noexcept : queues_{ queues } {}

    result<std::unique_ptr<lifetime_manager>> lifetime_manager::create(device_queues& queues)
    {
        return std::unique_ptr<lifetime_manager>{ new lifetime_manager{ queues } };
    }

    std::uint64_t lifetime_manager::list_opened() noexcept
    {
        return epochs_.enter();
    }

    void lifetime_manager::list_closed(const std::uint64_t epoch) noexcept
    {
        epochs_.leave(epoch);
    }

    void lifetime_manager::destroy(release_entry entry)
    {
        // The caller removed the object from its slot map first, so every list that can still reference it is open
        // now, in this epoch or the previous one (§23.7)
        releases_.park(epochs_.current(), std::move(entry));
        advance_and_tag();
    }

    void lifetime_manager::advance_and_tag()
    {
        // The epoch first: lists leave after their submission published last_submitted, so the values read next
        // cover every list that has left
        const std::uint64_t clean_before = epochs_.try_advance();
        releases_.tag_clean(clean_before, queues_.last_submitted());
    }

    bool lifetime_manager::housekeeping(const std::size_t limit) noexcept
    {
        const std::uint64_t epoch_before = epochs_.current();
        advance_and_tag();
        const deferred_release::collect_result collected = releases_.collect(queues_.completed(), limit);
        check_open_lists(epoch_before);
        return collected.more_ready;
    }

    void lifetime_manager::drain() noexcept
    {
        releases_.drain();
    }

    std::size_t lifetime_manager::pending() const noexcept
    {
        return releases_.pending();
    }

    const deferred_release& lifetime_manager::releases() const noexcept
    {
        return releases_;
    }

    const open_list_epochs& lifetime_manager::epochs() const noexcept
    {
        return epochs_;
    }

    void lifetime_manager::check_open_lists([[maybe_unused]] const std::uint64_t epoch_before) noexcept
    {
#ifndef NDEBUG
        const bool blocked = releases_.parked() > 0 && epochs_.open_lists() > 0 && epochs_.current() == epoch_before;
        if (!blocked)
        {
            blocked_since_.store(0, std::memory_order_relaxed);
            return;
        }

        const clock::rep now = clock::now().time_since_epoch().count();
        clock::rep since = 0;
        if (blocked_since_.compare_exchange_strong(since, now, std::memory_order_relaxed))
        {
            return; // just started
        }
        const auto interval = std::chrono::duration_cast<clock::duration>(open_list_warning_interval).count();
        clock::rep last = last_warning_.load(std::memory_order_relaxed);
        if (now - since <= interval || now - last <= interval ||
            !last_warning_.compare_exchange_strong(last, now, std::memory_order_relaxed))
        {
            return;
        }
        try
        {
            log::get("rhi")->warn(
                "lifetime_manager: {} destroyed object(s) held back for over a second by {} open command list(s). A "
                "list that is never submitted or discarded holds back every later release (09-rhi.md §23.7).",
                releases_.parked(),
                epochs_.open_lists());
        }
        catch (...)
        {}
#endif
    }
} // namespace avis::rhi::d3d12
