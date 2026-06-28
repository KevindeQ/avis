#ifndef AVIS_VIRTUAL_CLOCK_STATE_H
#define AVIS_VIRTUAL_CLOCK_STATE_H

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace avis
{
    class virtual_clock_state
    {
    public:
        using rep = std::int64_t;
        using period = std::nano;
        using duration = std::chrono::duration<rep, period>;

        virtual_clock_state() = default;

        duration now() noexcept;

        void advance(duration delta);
        
        void reset(duration new_time = duration::zero());

        void resume();
        void pause();

        void set_time(duration new_time);
        void set_scale(double scale);

    private:
        void update_from_real_time() noexcept;

        std::mutex mutex_;

        duration current_time_ = duration::zero();
        std::chrono::steady_clock::time_point last_real_ = std::chrono::steady_clock::now();

        double scale_ = 1.0;
        bool paused_ = true;
    };
} // namespace avis

#endif
