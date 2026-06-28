#ifndef AVIS_VIRTUAL_CLOCK_H
#define AVIS_VIRTUAL_CLOCK_H

#include "avis/virtual_clock_state.h"

#include <atomic>
#include <cassert>

namespace avis
{
    struct tls_state_policy
    {
    public:
        template<typename State>
        static State* get() noexcept
        {
            return slot<State>();
        }

        template<typename State>
        static State* set(State* state) noexcept
        {
            State* previous = slot<State>();
            slot<State>() = state;
            return previous;
        }

    private:
        template<typename State>
        static State*& slot() noexcept
        {
            static thread_local State* state = nullptr;
            return state;
        }
    };

    struct static_state_policy
    {
    public:
        template<typename State>
        static State* get() noexcept
        {
            return slot<State>().load(std::memory_order_acquire);
        }

        template<typename State>
        static State* set(State* state) noexcept
        {
            return slot<State>().exchange(state, std::memory_order_acq_rel);
        }

    private:
        template<typename State>
        static std::atomic<State*>& slot() noexcept
        {
            static std::atomic<State*> state = nullptr;
            return state;
        }
    };

    template<typename StoragePolicy, bool IsSteady = false>
    class basic_virtual_clock
    {
    public:
        using state_type = virtual_clock_state;

        using rep = state_type::rep;
        using period = state_type::period;
        using duration = state_type::duration;
        using time_point = std::chrono::time_point<basic_virtual_clock>;

        static constexpr bool is_steady = IsSteady;

        static time_point now() noexcept
        {
            auto* state = StoragePolicy::template get<state_type>();
            assert(state != nullptr && "No state bound to basic_virtual_clock");

            return time_point{ state->now() };
        }

        static void bind_state(state_type& state) noexcept
        {
            StoragePolicy::template set<state_type>(&state);
        }

        static void unbind_state() noexcept
        {
            StoragePolicy::template set<state_type>(nullptr);
        }

        static state_type* state() noexcept
        {
            return StoragePolicy::template get<state_type>();
        }

    private:
        basic_virtual_clock() = delete;
    };
} // namespace avis

#endif
