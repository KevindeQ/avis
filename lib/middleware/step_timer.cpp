#include "avis/middleware/step_timer.h"

step_timer::step_timer(const time_span_nanoseconds time_delta_ns) :
    time_delta{ time_delta_ns },
    time_delta_max{ 100ms },

    global_time{ 0ns },
    current_time{ clock::now() },
    accumulator{ 0ns },
    seconds_elapsed{ 0.0 },

    global_frame_count{ 0 },
    second_frame_count{ 0 },
    fps{ 0 }
{}

void step_timer::reset()
{
    current_time = clock::now();
    seconds_elapsed = 0.0f;

    second_frame_count = 0;
    fps = 0;
}

float step_timer::tick_residual() const
{
    if (time_delta == 0ns)
    {
        return 0.0f;
    }

    return static_cast<float>(accumulator.count()) / static_cast<float>(time_delta.count());
}

double step_timer::elapsed_seconds() const
{
    return seconds_elapsed;
}

std::uint64_t step_timer::frame_count() const
{
    return global_frame_count;
}

std::uint32_t step_timer::frames_per_second() const
{
    return fps;
}

std::chrono::nanoseconds
    step_timer::duration_in_nanoseconds(const time_point& start_time, const time_point& end_time)
{
    return std::chrono::duration_cast<time_span_nanoseconds>(end_time - start_time);
}

double step_timer::duration_in_seconds(const time_span_nanoseconds& duration)
{
    return time_span_seconds(duration).count();
}
