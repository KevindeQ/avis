#ifndef MIDDLEWARE_INPUT_INPUT_STATE_H
#define MIDDLEWARE_INPUT_INPUT_STATE_H

#include "avis/core/common.h"

namespace input
{
    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    class input_state
    {
    public:
        void add_input(const actions_t& action);
        void add_input(const states_t& state);
        void add_input(const ranges_t& range);

        void clear();

        bool contains(const actions_t action) const;
        bool contains(const states_t state) const;
        bool contains(const ranges_t range) const;

    private:
        std::unordered_set<actions_t> actions;
        std::unordered_set<states_t> states;
        std::unordered_map<ranges_t, double> ranges;
    };

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_state<actions_t, states_t, ranges_t>::add_input(const actions_t& action)
    {
        actions.insert(action);
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_state<actions_t, states_t, ranges_t>::add_input(const states_t& state)
    {
        states.insert(state);
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_state<actions_t, states_t, ranges_t>::add_input(const ranges_t& range)
    {
        // TODO: Actually pass value as parameter
        ranges.insert_or_assign(range, 1.0);
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_state<actions_t, states_t, ranges_t>::clear()
    {
        actions.clear();
        states.clear();
        ranges.clear();
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    bool input_state<actions_t, states_t, ranges_t>::contains(const actions_t action) const
    {
        return actions.contains(action);
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    bool input_state<actions_t, states_t, ranges_t>::contains(const states_t state) const
    {
        return states.contains(state);
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    bool input_state<actions_t, states_t, ranges_t>::contains(const ranges_t range) const
    {
        return ranges.contains(range);
    }
} // namespace input

#endif
