#include "avis/scene/components/driver.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <utility>

namespace avis::scene::components
{
    // ---- properties ----

    std::size_t properties::lower_bound(const std::string_view key) const noexcept
    {
        const auto found = std::ranges::lower_bound(
            index_,
            key,
            std::less<>{},
            [this](const std::uint32_t at) -> std::string_view { return entries_[at].key; });
        return static_cast<std::size_t>(found - index_.begin());
    }

    void properties::set(const std::string_view key, property_value value, const std::string_view unit)
    {
        const std::size_t slot = lower_bound(key);
        if (slot < index_.size() && entries_[index_[slot]].key == key)
        {
            property_entry& entry = entries_[index_[slot]];
            entry.value = std::move(value);
            entry.unit.assign(unit);
            return;
        }

        // Reserve both first, so a failed allocation leaves the two consistent
        entries_.reserve(entries_.size() + 1);
        index_.reserve(index_.size() + 1);
        entries_.push_back({ .key = std::string{ key }, .value = std::move(value), .unit = std::string{ unit } });
        index_.insert(
            index_.begin() + static_cast<std::ptrdiff_t>(slot), static_cast<std::uint32_t>(entries_.size() - 1));
    }

    bool properties::remove(const std::string_view key)
    {
        const std::size_t slot = lower_bound(key);
        if (slot == index_.size() || entries_[index_[slot]].key != key)
        {
            return false;
        }
        const std::uint32_t position = index_[slot];
        entries_.erase(entries_.begin() + position);
        index_.erase(index_.begin() + static_cast<std::ptrdiff_t>(slot));

        // The entries after the removed one moved down by one; the keys' order is unchanged
        for (std::uint32_t& at : index_)
        {
            if (at > position)
            {
                --at;
            }
        }
        return true;
    }

    const property_entry* properties::find(const std::string_view key) const noexcept
    {
        const std::size_t slot = lower_bound(key);
        if (slot < index_.size() && entries_[index_[slot]].key == key)
        {
            return &entries_[index_[slot]];
        }
        return nullptr;
    }

    // ---- belief_geometry ----

    namespace
    {
        auto chunk_position(std::vector<geometry_chunk>& chunks, const std::uint32_t id) noexcept
        {
            return std::ranges::lower_bound(chunks, id, std::less<>{}, &geometry_chunk::id);
        }
    } // namespace

    void belief_geometry::replace(geometry_chunk chunk)
    {
        const auto found = chunk_position(chunks_, chunk.id);
        if (found != chunks_.end() && found->id == chunk.id)
        {
            *found = std::move(chunk);
            return;
        }
        chunks_.insert(found, std::move(chunk));
    }

    bool belief_geometry::remove(const std::uint32_t id)
    {
        const auto found = chunk_position(chunks_, id);
        if (found == chunks_.end() || found->id != id)
        {
            return false;
        }
        chunks_.erase(found);
        return true;
    }

    const geometry_chunk* belief_geometry::find(const std::uint32_t id) const noexcept
    {
        const auto found = std::ranges::lower_bound(chunks_, id, std::less<>{}, &geometry_chunk::id);
        return found != chunks_.end() && found->id == id ? &*found : nullptr;
    }

    // ---- lidar_points ----

    void lidar_points::push(lidar_sweep sweep)
    {
        const std::size_t kept = std::max<std::size_t>(retention, 1);
        if (sweeps.size() >= kept)
        {
            // Drop the oldest so that the new one makes `kept`
            sweeps.erase(sweeps.begin(), sweeps.begin() + static_cast<std::ptrdiff_t>(sweeps.size() - kept + 1));
        }
        sweeps.push_back(std::move(sweep));
    }
} // namespace avis::scene::components
