#include "avis/rhi/d3d12/deferred_release.h"

#include <algorithm>
#include <mutex>

namespace avis::rhi::d3d12
{
    void deferred_release::park(const std::uint64_t epoch, release_entry entry)
    {
        const std::lock_guard lock{ mutex_ };
        // A destroy that read an older epoch than one parked after it joins the later group: parking later is only
        // conservative, and the groups stay sorted
        if (parked_.empty() || parked_.back().first < epoch)
        {
            parked_.emplace_back(epoch, std::vector<release_entry>{});
        }
        parked_.back().second.push_back(std::move(entry));
        parked_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void deferred_release::tag_clean(const std::uint64_t clean_before, const release_tag& last_submitted)
    {
        const std::lock_guard lock{ mutex_ };
        while (!parked_.empty() && parked_.front().first < clean_before)
        {
            std::vector<release_entry>& entries = parked_.front().second;
            const std::size_t count = entries.size();
            if (!tagged_.empty() && tagged_.back().first == last_submitted)
            {
                std::vector<release_entry>& group = tagged_.back().second;
                group.insert(
                    group.end(), std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
            }
            else
            {
                tagged_.emplace_back(last_submitted, std::move(entries));
            }
            parked_.pop_front();
            parked_count_.fetch_sub(count, std::memory_order_relaxed);
            tagged_count_.fetch_add(count, std::memory_order_relaxed);
        }
    }

    deferred_release::collect_result deferred_release::collect(const release_tag& completed, const std::size_t limit)
    {
        collect_result result;
        std::vector<release_entry> ready;
        {
            const std::lock_guard lock{ mutex_ };
            // Tags grow on every queue, so the first group that isn't reached ends the search
            while (!tagged_.empty() && ready.size() < limit && reached(tagged_.front().first, completed))
            {
                std::vector<release_entry>& group = tagged_.front().second;
                const std::size_t take = std::min(limit - ready.size(), group.size());
                ready.insert(
                    ready.end(),
                    std::make_move_iterator(group.end() - static_cast<std::ptrdiff_t>(take)),
                    std::make_move_iterator(group.end()));
                group.erase(group.end() - static_cast<std::ptrdiff_t>(take), group.end());
                if (group.empty())
                {
                    tagged_.pop_front();
                }
            }
            tagged_count_.fetch_sub(ready.size(), std::memory_order_relaxed);
            result.more_ready = !tagged_.empty() && reached(tagged_.front().first, completed);
        }
        result.freed = ready.size();
        // `ready` is destroyed here, outside the mutex: freeing never nests in the release lock (§19.4)
        return result;
    }

    void deferred_release::drain() noexcept
    {
        std::deque<std::pair<std::uint64_t, std::vector<release_entry>>> parked;
        std::deque<std::pair<release_tag, std::vector<release_entry>>> tagged;
        {
            const std::lock_guard lock{ mutex_ };
            parked.swap(parked_);
            tagged.swap(tagged_);
            parked_count_.store(0, std::memory_order_relaxed);
            tagged_count_.store(0, std::memory_order_relaxed);
        }
    }

    std::size_t deferred_release::pending() const noexcept
    {
        return parked() + tagged();
    }

    std::size_t deferred_release::parked() const noexcept
    {
        return parked_count_.load(std::memory_order_relaxed);
    }

    std::size_t deferred_release::tagged() const noexcept
    {
        return tagged_count_.load(std::memory_order_relaxed);
    }

    bool deferred_release::reached(const release_tag& tag, const release_tag& completed) noexcept
    {
        return tag[0] <= completed[0] && tag[1] <= completed[1] && tag[2] <= completed[2];
    }
} // namespace avis::rhi::d3d12
