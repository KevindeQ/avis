#ifndef AVIS_VISUALIZER_SIMULATION_COMMAND_QUEUE_H
#define AVIS_VISUALIZER_SIMULATION_COMMAND_QUEUE_H

// The GUI → simulation path (docs/design/12-visualizer.md §4): many producers push, one consumer drains everything
// at once and otherwise sleeps on a condition variable until a value, a deadline or a stop request.

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

namespace avis::visualizer::simulation
{
    template<typename T>
    class command_queue
    {
    public:
        // Any thread
        void push(T value)
        {
            {
                const std::lock_guard lock{ mutex_ };
                values_.push_back(std::move(value));
            }
            ready_.notify_one();
        }

        // The consumer thread. Waits until there's a value, `deadline` passes or stop is requested, then moves every
        // value into `out` (cleared first), oldest first. Returns false when stop was requested.
        bool wait_and_drain(
            std::vector<T>& out, const std::chrono::steady_clock::time_point deadline, const std::stop_token& stop)
        {
            out.clear();
            std::unique_lock lock{ mutex_ };
            ready_.wait_until(lock, stop, deadline, [this] { return !values_.empty(); });
            std::swap(out, values_);
            return !stop.stop_requested();
        }

    private:
        std::mutex mutex_;
        std::condition_variable_any ready_;
        std::vector<T> values_;
    };
} // namespace avis::visualizer::simulation

#endif
