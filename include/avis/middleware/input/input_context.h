#ifndef MIDDLEWARE_INPUT_INPUT_CONTEXT_H
#define MIDDLEWARE_INPUT_INPUT_CONTEXT_H

#include "avis/core/common.h"
#include "avis/middleware/input/details/input_type_id.h"
#include "avis/middleware/input/details/input_wrapper.h"
#include "avis/middleware/input/input_state.h"

namespace input
{
    using input_context_collector = std::function<bool(const details::input_wrapper& raw_input)>;

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    class input_context
    {
    public:
        template<typename raw_input_t>
        void add_mapping(raw_input_t raw_value, actions_t action);
        template<typename raw_input_t>
        void add_mapping(raw_input_t raw_value, states_t state);
        template<typename raw_input_t, typename value_t>
        void add_mapping(raw_input_t raw_value, ranges_t range /*, range_converter_t range_converter*/);

        input_context_collector
            build_context_collector(input_state<actions_t, states_t, ranges_t>& current_state) const;

    private:
        using store_value_t = std::function<void(input_state<actions_t, states_t, ranges_t>& current_state)>;

        /*std::unordered_map<details::input_wrapper, store_value_t> conversion_map;*/
        std::vector<details::input_wrapper> index_map;
        std::vector<store_value_t> collector_map;
    };

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    template<typename raw_input_t>
    void input_context<actions_t, states_t, ranges_t>::add_mapping(raw_input_t raw_value, actions_t action)
    {
        /*conversion_map.insert_or_assign(
            details::input_wrapper{ raw_value },
            [action](input_state<actions_t, states_t, ranges_t>& current_state) { current_state.add_input(action); });*/

        index_map.push_back(details::input_wrapper{ raw_value });
        collector_map.push_back([action](input_state<actions_t, states_t, ranges_t>& current_state)
                                { current_state.add_input(action); });
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    template<typename raw_input_t>
    void input_context<actions_t, states_t, ranges_t>::add_mapping(raw_input_t raw_value, states_t state)
    {
        /*conversion_map.insert_or_assign(
            details::input_wrapper{ raw_value },
            [state](input_state<actions_t, states_t, ranges_t>& current_state) { current_state.add_input(state); });*/

        index_map.push_back(details::input_wrapper{ raw_value });
        collector_map.push_back([state](input_state<actions_t, states_t, ranges_t>& current_state)
                                { current_state.add_input(state); });
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    template<typename raw_input_t, typename value_t>
    void input_context<actions_t, states_t, ranges_t>::add_mapping(
        raw_input_t raw_value, ranges_t range /*, range_converter_t range_converter*/)
    {
        /*conversion_map.insert_or_assign(
            details::input_wrapper{ raw_value },
            [range](input_state<actions_t, states_t, ranges_t>& current_state) { current_state.add_input(range); });*/

        index_map.push_back(details::input_wrapper{ raw_value });
        collector_map.push_back([range](input_state<actions_t, states_t, ranges_t>& current_state)
                                { current_state.add_input(range); });
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    input_context_collector input_context<actions_t, states_t, ranges_t>::build_context_collector(
        input_state<actions_t, states_t, ranges_t>& current_state) const
    {
        return [this, &current_state](const details::input_wrapper& raw_input)
        {
            /*typename decltype(conversion_map)::const_iterator it = conversion_map.find(raw_input);
            if (it == conversion_map.cend())
            {
                return false;
            }

            it->second(current_state);
            return true;*/

            const auto& it = std::find(index_map.cbegin(), index_map.cend(), raw_input);
            if (it == index_map.cend())
            {
                return false;
            }

            std::size_t index = std::distance(index_map.cbegin(), it);
            collector_map[index](current_state);
            return true;
        };
    }
} // namespace input

#endif
