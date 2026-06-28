#include "avis/virtual_clock_state.h"

namespace avis
{
    virtual_clock_state::duration virtual_clock_state::now() noexcept
    {
        std::scoped_lock lock{ mutex_ };
        update_from_real_time();

        return current_time_;
    }

    void virtual_clock_state::advance(duration delta)
    {
        if (delta < duration::zero())
        {
            throw std::invalid_argument("virtual_clock_state::advance requires non-negative duration");
        }

        std::scoped_lock lock{ mutex_ };
        current_time_ += delta;
    }

    void virtual_clock_state::reset(duration new_time)
    {
        std::scoped_lock lock{ mutex_ };
        current_time_ = new_time;
        last_real_ = std::chrono::steady_clock::now();
    }

    void virtual_clock_state::set_time(duration new_time)
    {
        std::scoped_lock lock{ mutex_ };
        current_time_ = new_time;
        last_real_ = std::chrono::steady_clock::now();
    }

    void virtual_clock_state::resume()
    {
        std::scoped_lock lock{ mutex_ };

        if (!paused_)
        {
            return;
        }

        paused_ = false;
        last_real_ = std::chrono::steady_clock::now();
    }

    void virtual_clock_state::pause()
    {
        std::scoped_lock lock{ mutex_ };
        update_from_real_time();
        paused_ = true;
    }

    void virtual_clock_state::set_scale(double scale)
    {
        if (scale < 0.0)
        {
            throw std::invalid_argument("virtual_clock_state::set_scale requires non-negative scale");
        }

        std::scoped_lock lock{ mutex_ };

        update_from_real_time();
        scale_ = scale;
    }

    void virtual_clock_state::update_from_real_time() noexcept
    {
        if (paused_)
        {
            return;
        }

        const auto real_now = std::chrono::steady_clock::now();
        const auto real_delta = real_now - last_real_;

        const auto scaled_delta = std::chrono::duration_cast<duration>(real_delta * scale_);

        current_time_ += scaled_delta;
        last_real_ = real_now;
    }
}
