#ifndef AVIS_SIMULATION_TIME_H
#define AVIS_SIMULATION_TIME_H

// Simulation time types (docs/design/03-simulation-core.md §2, §5.2). Only the types the scene needs; the clock state
// machine (03 §3) comes with the simulation core.

#include "avis/virtual_clock.h"

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace avis::simulation
{
    // The std::chrono facade over the simulation thread's virtual_clock_state (03 §2): now() is the time of the current
    // committed tick
    using simulation_clock = basic_virtual_clock<tls_state_policy>;

    using simulation_duration = std::chrono::nanoseconds; // int64
    using simulation_time_point = std::chrono::time_point<simulation_clock, simulation_duration>;
    using step_index = std::uint64_t;

    static_assert(std::is_same_v<simulation_clock::duration, simulation_duration>);

    // t_k = k * delta_time, exactly; never accumulated
    constexpr simulation_duration time_of_step(const step_index k, const simulation_duration delta_time) noexcept
    {
        return delta_time * static_cast<std::int64_t>(k);
    }

    // Presentation time anchor published with each snapshot (03 §5.2):
    // t_render = t_base + (wall_now - wall_base) * rate. Presentation only; never feeds back into simulation state.
    struct time_anchor
    {
        simulation_duration t_base{};
        std::chrono::steady_clock::time_point wall_base{};
        double rate = 0.0;
    };
} // namespace avis::simulation

#endif
