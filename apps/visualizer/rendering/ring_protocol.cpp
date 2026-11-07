#include "rendering/ring_protocol.h"

#include "avis/detail/check.h"

#include <algorithm>
#include <string_view>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "visualizer";
    } // namespace

    void ring_protocol::catch_up(const std::uint64_t produced)
    {
        const std::uint64_t latest_frame = latest_.load() >> 2;
        std::optional<std::uint32_t> newest;
        for (std::uint32_t slot = 0; slot < ring_slot_count; ++slot)
        {
            const std::uint64_t frame = frames_[slot];
            if (frame > latest_frame && frame <= produced && (!newest || frame > frames_[*newest]))
            {
                newest = slot;
            }
        }
        if (newest)
        {
            latest_.store(pack(frames_[*newest], *newest));
        }
    }

    slot_acquisition ring_protocol::try_acquire_slot(const ring_fence_values values)
    {
        const std::scoped_lock lock{ avis_mutex_ };
        const ring_wait next_use{ .fence = ring_fence::consumed, .value = values.consumed + 1 };
        if (!AVIS_CHECK(logger_name, !reserved_, "ring_protocol: a slot is already reserved") ||
            !AVIS_CHECK(logger_name, !retired_, "ring_protocol: try_acquire_slot on a retired ring"))
        {
            return { .wait = next_use };
        }

        catch_up(values.produced);
        // Read after latest_ was written (catch_up, publish): either this sees Qt's showing_ and picked_, or Qt's
        // re-read of latest_ in qt_pick sees the newer frame and Qt picks that one instead
        const std::uint64_t latest_frame = latest_.load() >> 2;
        const std::int32_t showing = showing_.load();

        // A slot Avis is writing (frame above the latest) or that holds the latest frame
        const auto avis_owned = [&](const std::uint32_t slot)
        { return frames_[slot] != 0 && frames_[slot] >= latest_frame; };

        std::optional<std::uint32_t> chosen;
        for (std::uint32_t slot = 0; slot < ring_slot_count; ++slot)
        {
            if (avis_owned(slot) || showing == static_cast<std::int32_t>(slot) ||
                picked_[slot].load() > values.consumed)
            {
                continue;
            }
            if (!chosen || frames_[slot] < frames_[*chosen])
            {
                chosen = slot;
            }
        }
        if (chosen)
        {
            reserved_ = chosen;
            return { .slot = chosen };
        }

        // Avis's own oldest frame in flight: once complete, it is published and frees the slot it replaces
        std::optional<std::uint64_t> in_flight;
        for (std::uint32_t slot = 0; slot < ring_slot_count; ++slot)
        {
            if (frames_[slot] > latest_frame)
            {
                in_flight = std::min(in_flight.value_or(frames_[slot]), frames_[slot]);
            }
        }
        if (in_flight)
        {
            return { .wait = { .fence = ring_fence::produced, .value = *in_flight } };
        }

        // Otherwise a slot Qt still samples, other than the one it shows
        std::optional<std::uint64_t> held;
        std::uint64_t newest_use = 0;
        for (std::uint32_t slot = 0; slot < ring_slot_count; ++slot)
        {
            const std::uint64_t picked = picked_[slot].load();
            newest_use = std::max(newest_use, picked);
            if (!avis_owned(slot) && showing != static_cast<std::int32_t>(slot) && picked > values.consumed)
            {
                held = std::min(held.value_or(picked), picked);
            }
        }
        if (held)
        {
            return { .wait = { .fence = ring_fence::consumed, .value = *held } };
        }

        // Qt shows one slot and the latest frame is in another: Qt's next render picks the latest, which frees the slot
        // it showed once that render's use is consumed
        return { .wait = { .fence = ring_fence::consumed, .value = std::max(newest_use, values.consumed) + 1 } };
    }

    void ring_protocol::bind(const std::uint32_t slot, const std::uint64_t frame)
    {
        const std::scoped_lock lock{ avis_mutex_ };
        if (!AVIS_CHECK(logger_name, reserved_ == slot, "ring_protocol::bind: the slot isn't reserved"))
        {
            return;
        }
        AVIS_CHECK(
            logger_name,
            frame > (latest_.load() >> 2) &&
                std::ranges::all_of(frames_, [&](const std::uint64_t bound) { return bound < frame; }),
            "ring_protocol::bind: frames must increase");
        frames_[slot] = frame;
        reserved_.reset();
    }

    void ring_protocol::release(const std::uint32_t slot)
    {
        const std::scoped_lock lock{ avis_mutex_ };
        if (reserved_ == slot)
        {
            reserved_.reset();
        }
    }

    bool ring_protocol::publish(const std::uint64_t frame, const std::uint32_t slot)
    {
        {
            const std::scoped_lock lock{ avis_mutex_ };
            // The slot may already hold a newer frame: catch_up published this one, and a later frame reused the slot
            if (slot < ring_slot_count && frames_[slot] == frame && frame > (latest_.load() >> 2))
            {
                latest_.store(pack(frame, slot));
            }
        }
        return !update_pending_.exchange(true);
    }

    std::optional<ring_frame> ring_protocol::latest_completed() const noexcept
    {
        const std::uint64_t latest = latest_.load();
        if (latest == 0)
        {
            return std::nullopt;
        }
        return ring_frame{ .frame = latest >> 2, .slot = static_cast<std::uint32_t>(latest & 3) };
    }

    void ring_protocol::retire()
    {
        const std::scoped_lock lock{ avis_mutex_ };
        retired_ = true;
        qt_state expected = qt_state::fresh;
        state_.compare_exchange_strong(expected, qt_state::retired_unattached);
    }

    release_check ring_protocol::check_released(const ring_fence_values values) const
    {
        const std::scoped_lock lock{ avis_mutex_ };
        const ring_wait next_use{ .fence = ring_fence::consumed, .value = values.consumed + 1 };
        if (!AVIS_CHECK(logger_name, retired_, "ring_protocol::check_released: the ring isn't retired") ||
            !AVIS_CHECK(logger_name, !reserved_, "ring_protocol::check_released: a slot is still reserved"))
        {
            return { .wait = next_use };
        }

        const std::uint64_t newest_frame = std::ranges::max(frames_);
        if (values.produced < newest_frame)
        {
            return { .wait = { .fence = ring_fence::produced, .value = newest_frame } };
        }

        switch (state_.load())
        {
        case qt_state::retired_unattached:
            return { .released = true };
        case qt_state::detached:
            {
                const std::uint64_t final_use = final_use_.load();
                if (values.consumed >= final_use)
                {
                    return { .released = true };
                }
                return { .wait = { .fence = ring_fence::consumed, .value = final_use } };
            }
        case qt_state::fresh:
        case qt_state::attached:
            break;
        }
        // Still attached: Qt's next use, from a pick or the detach, is above every pick so far
        std::uint64_t newest_use = 0;
        for (const std::atomic<std::uint64_t>& picked : picked_)
        {
            newest_use = std::max(newest_use, picked.load());
        }
        return { .wait = { .fence = ring_fence::consumed, .value = std::max(newest_use, values.consumed) + 1 } };
    }

    bool ring_protocol::qt_attach() noexcept
    {
        qt_state expected = qt_state::fresh;
        return state_.compare_exchange_strong(expected, qt_state::attached);
    }

    std::optional<qt_pick_result> ring_protocol::qt_pick() noexcept
    {
        // Cleared first: a publish after this wakes Qt again
        update_pending_.store(false);
        for (;;)
        {
            const std::uint64_t latest = latest_.load();
            if (latest == 0)
            {
                return std::nullopt;
            }
            const auto slot = static_cast<std::uint32_t>(latest & 3);
            const std::uint64_t use = ++qt_uses_;
            picked_[slot].store(use);
            showing_.store(static_cast<std::int32_t>(slot));
            // Unchanged: Avis can't take the slot any more (it sees showing_ or picked_). Changed: the slot may be
            // reused already, so take the newer frame. The earlier pick stays recorded; this render's use covers it.
            if (latest_.load() == latest)
            {
                return qt_pick_result{ .frame = latest >> 2, .slot = slot, .use = use };
            }
        }
    }

    std::uint64_t ring_protocol::qt_detach() noexcept
    {
        const std::uint64_t use = ++qt_uses_;
        final_use_.store(use);
        showing_.store(-1);
        state_.store(qt_state::detached);
        return use;
    }

    std::optional<std::uint32_t> ring_protocol::showing() const noexcept
    {
        const std::int32_t showing = showing_.load();
        if (showing < 0)
        {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(showing);
    }

    std::uint64_t ring_protocol::picked(const std::uint32_t slot) const noexcept
    {
        return slot < ring_slot_count ? picked_[slot].load() : 0;
    }

    bool ring_protocol::update_pending() const noexcept
    {
        return update_pending_.load();
    }
} // namespace avis::visualizer::rendering
