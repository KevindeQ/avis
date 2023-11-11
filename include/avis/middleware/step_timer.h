#ifndef MIDDLEWARE_STEP_TIMER_H
#define MIDDLEWARE_STEP_TIMER_H

#include "avis/common.h"

class StepTimer
{
private:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    using TimeSpanSeconds = std::chrono::duration<Float64>;
    using TimeSpanNanoseconds = std::chrono::nanoseconds;

public:
    StepTimer::StepTimer(const TimeSpanNanoseconds FixedTimeStep) :
        m_FixedTimeStep(FixedTimeStep),
        m_MaxTimeStep(100ms),

        m_GlobalTime(0ns),
        m_CurrentTime(Clock::now()),
        m_Accumulator(0ns),
        m_ElapsedSeconds(0.0),

        m_GlobalFrameCount(0),
        m_SecondFrameCount(0),
        m_FramesPerSecond(0)
    {}

    // void Tick(void(&Update)(const StepTimer&));
    template<typename UpdateFunc>
    void Tick(const UpdateFunc& Update)
    {
        // Update time
        const auto NewTime = Clock::now();
        const auto ElapsedTime = std::min(DurationInNanoseconds(m_CurrentTime, NewTime), m_MaxTimeStep);
        m_CurrentTime = NewTime;

        m_ElapsedSeconds = DurationInSeconds(ElapsedTime);

        Uint64 FrameCount = 0;
        if (m_FixedTimeStep > 0ns)
        {
            m_Accumulator += ElapsedTime;
            while (m_Accumulator >= m_FixedTimeStep)
            {
                Update(*this);

                m_GlobalTime += m_FixedTimeStep;
                m_Accumulator -= m_FixedTimeStep;

                FrameCount += 1;
            }
        }
        else
        {
            Update(*this);

            m_GlobalTime += ElapsedTime;
            m_Accumulator = 0ns;

            FrameCount += 1;
        }

        // Update frame count/frames per seconds tracking values
        m_GlobalFrameCount += FrameCount;
        m_SecondFrameCount += FrameCount;
        m_SecondAccumulator += ElapsedTime;

        static const TimeSpanNanoseconds Second = 1s;
        if (m_SecondAccumulator >= Second)
        {
            m_FramesPerSecond = m_SecondFrameCount;
            m_SecondFrameCount = 0;
            m_SecondAccumulator %= Second;
        }
    }

    void Reset()
    {
        m_CurrentTime = Clock::now();
        m_ElapsedSeconds = 0.0f;

        m_SecondFrameCount = 0;
        m_FramesPerSecond = 0;
    }

    Float32 GetTickResidual() const
    {
        if (m_FixedTimeStep == 0ns)
        {
            return 0.0f;
        }

        return static_cast<Float32>(m_Accumulator.count()) / static_cast<Float32>(m_FixedTimeStep.count());
    }

    Float64 GetElapsedSeconds() const
    {
        return m_ElapsedSeconds;
    }

    Uint64 GetFrameCount() const
    {
        return m_GlobalFrameCount;
    }

    Uint32 GetFramesPerSecond() const
    {
        return m_FramesPerSecond;
    }

private:
    static std::chrono::nanoseconds DurationInNanoseconds(const TimePoint& StartTime, const TimePoint& EndTime)
    {
        return std::chrono::duration_cast<TimeSpanNanoseconds>(EndTime - StartTime);
    }

    static Float64 DurationInSeconds(const TimeSpanNanoseconds& Duration)
    {
        return TimeSpanSeconds(Duration).count();
    }

private:
    const TimeSpanNanoseconds m_FixedTimeStep;
    const TimeSpanNanoseconds m_MaxTimeStep;

    TimeSpanNanoseconds m_GlobalTime;
    TimePoint m_CurrentTime;
    TimeSpanNanoseconds m_Accumulator;
    Float64 m_ElapsedSeconds;

    Uint64 m_GlobalFrameCount;
    Uint32 m_SecondFrameCount;
    Uint32 m_FramesPerSecond;
    TimeSpanNanoseconds m_SecondAccumulator;
};

#endif
