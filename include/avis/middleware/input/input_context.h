#ifndef MIDDLEWARE_INPUT_INPUT_CONTEXT_H
#define MIDDLEWARE_INPUT_INPUT_CONTEXT_H

#include "avis/core/common.h"
#include "avis/middleware/input/details/input_type_id.h"
#include "avis/middleware/input/details/input_wrapper.h"
#include "avis/middleware/input/input_state.h"

namespace input
{
    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    class input_context
    {
    public:
        template<typename raw_input_t>
        void add_mapping(raw_input_t raw_value, actions_t action);
        template<typename raw_input_t>
        void add_mapping(raw_input_t raw_value, states_t state);
        template<typename raw_input_t>
        void add_mapping(raw_input_t raw_value, ranges_t range);

        bool map_value(const details::input_wrapper& raw_value, input_state<actions_t, states_t, ranges_t>& inputs) const;

    private:
        std::unordered_map<details::input_wrapper, actions_t> actions_map;
        std::unordered_map<details::input_wrapper, states_t> states_map;
        std::unordered_map<details::input_wrapper, ranges_t> ranges_map;
    };

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    template<typename raw_input_t>
    void input_context<actions_t, states_t, ranges_t>::add_mapping(raw_input_t raw_value, actions_t action)
    {
        actions_map.insert_or_assign(details::input_wrapper{ raw_value }, action);
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    template<typename raw_input_t>
    void input_context<actions_t, states_t, ranges_t>::add_mapping(raw_input_t raw_value, states_t state)
    {
        states_map.insert_or_assign(details::input_wrapper{ raw_value }, state);
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    template<typename raw_input_t>
    void input_context<actions_t, states_t, ranges_t>::add_mapping(raw_input_t raw_value, ranges_t range)
    {
        ranges_map.insert_or_assign(details::input_wrapper{ raw_value }, range);
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    bool input_context<actions_t, states_t, ranges_t>::map_value(
        const details::input_wrapper& raw_value, input_state<actions_t, states_t, ranges_t>& inputs) const
    {
        typename std::unordered_map<details::input_wrapper, actions_t>::const_iterator it_action =
            actions_map.find(raw_value);
        if (it_action != actions_map.cend())
        {
            // TODO: Do something with action
            inputs.add_input(it_action->second);
            return true;
        }

        typename std::unordered_map<details::input_wrapper, states_t>::const_iterator it_state =
            states_map.find(raw_value);
        if (it_state != states_map.cend())
        {
            // TODO: Do something with state
            inputs.add_input(it_state->second);
            return true;
        }

        typename std::unordered_map<details::input_wrapper, ranges_t>::const_iterator it_range =
            ranges_map.find(raw_value);
        if (it_range != ranges_map.cend())
        {
            // TODO: Do something with range
            return true;
        }

        return false;
    }
} // namespace input

#endif
