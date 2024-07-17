#ifndef MIDDLEWARE_STEP_TIMER_H
#define MIDDLEWARE_STEP_TIMER_H

#include "avis/core/common.h"

class step_timer
{
private:
    using clock = std::chrono::high_resolution_clock;
    using time_point = clock::time_point;
    using time_span_seconds = std::chrono::duration<double>;
    using time_span_nanoseconds = std::chrono::nanoseconds;

public:
    step_timer(const time_span_nanoseconds time_delta_ns);

    // void tick(void(&update)(const step_timer&));
    template<typename update_func>
    void tick(const update_func& update);

    void reset();

    float tick_residual() const;

    double elapsed_seconds() const;
    std::uint64_t frame_count() const;
    std::uint32_t frames_per_second() const;

private:
    static std::chrono::nanoseconds duration_in_nanoseconds(const time_point& start_time, const time_point& end_time);
    static double duration_in_seconds(const time_span_nanoseconds& duration);

private:
    const time_span_nanoseconds time_delta;
    const time_span_nanoseconds time_delta_max;

    time_span_nanoseconds global_time;
    time_point current_time;
    time_span_nanoseconds accumulator;
    double seconds_elapsed;

    std::uint64_t global_frame_count;
    std::uint32_t second_frame_count;
    std::uint32_t fps;
    time_span_nanoseconds second_accumulator;
};

template<typename update_func>
void step_timer::tick(const update_func& update)
{
    // Update time
    const auto new_time = clock::now();
    const auto elapsed_time = std::min(duration_in_nanoseconds(current_time, new_time), time_delta_max);
    current_time = new_time;

    seconds_elapsed = duration_in_seconds(elapsed_time);

    std::uint64_t frame_count = 0;
    if (time_delta > 0ns)
    {
        accumulator += elapsed_time;
        while (accumulator >= time_delta)
        {
            update(*this);

            global_time += time_delta;
            accumulator -= time_delta;

            frame_count += 1;
        }
    }
    else
    {
        update(*this);

        global_time += elapsed_time;
        accumulator = 0ns;

        frame_count += 1;
    }

    // Update frame count/frames per seconds tracking values
    global_frame_count += frame_count;
    second_frame_count += frame_count;
    second_accumulator += elapsed_time;

    if (second_accumulator >= 1s)
    {
        fps = second_frame_count;
        second_frame_count = 0;
        second_accumulator %= 1s;
    }
}

#endif
