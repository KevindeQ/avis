#ifndef MIDDLEWARE_INPUT_INPUT_CONTEXT_H
#define MIDDLEWARE_INPUT_INPUT_CONTEXT_H

#include "avis/core/common.h"
#include "avis/middleware/input/details/input_type_id.h"
#include "avis/middleware/input/details/input_wrapper.h"
#include "avis/middleware/input/input_state.h"

namespace input
{
    using input_tag_collector = std::function<bool(const details::input_wrapper& raw_input)>;
    using input_value_collector = std::function<bool(const details::input_wrapper& raw_input, const double value)>;

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    class input_context
    {
    public:
        template<typename raw_input_t>
        void add_mapping(raw_input_t raw_value, actions_t action);
        template<typename raw_input_t>
        void add_mapping(raw_input_t raw_value, states_t state);
        template<typename raw_input_t/*, typename value_t*/>
        void add_mapping(raw_input_t raw_value, ranges_t range /*, range_converter_t range_converter*/);

        input_tag_collector
            build_tag_collector(input_state<actions_t, states_t, ranges_t>& current_state) const;
        input_value_collector build_value_collector(input_state<actions_t, states_t, ranges_t>& current_state) const;

    private:
        using store_value_t = std::function<void(
            input_state<actions_t, states_t, ranges_t>& current_state, const std::optional<double>& value)>;

        std::vector<details::input_wrapper> index_map;
        std::vector<store_value_t> collector_map;
    };

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    template<typename raw_input_t>
    void input_context<actions_t, states_t, ranges_t>::add_mapping(raw_input_t raw_value, actions_t action)
    {
        index_map.push_back(details::input_wrapper{ raw_value });
        collector_map.push_back(
            [action](input_state<actions_t, states_t, ranges_t>& current_state, const std::optional<double>& value)
            { current_state.add_input(action); });
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    template<typename raw_input_t>
    void input_context<actions_t, states_t, ranges_t>::add_mapping(raw_input_t raw_value, states_t state)
    {
        index_map.push_back(details::input_wrapper{ raw_value });
        collector_map.push_back(
            [state](input_state<actions_t, states_t, ranges_t>& current_state, const std::optional<double>& value)
            { current_state.add_input(state); });
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    template<typename raw_input_t/*, typename value_t*/>
    void input_context<actions_t, states_t, ranges_t>::add_mapping(
        raw_input_t raw_value, ranges_t range /*, range_converter_t range_converter*/)
    {
        index_map.push_back(details::input_wrapper{ raw_value });
        collector_map.push_back(
            [range](input_state<actions_t, states_t, ranges_t>& current_state, const std::optional<double>& value)
            {
                if (value.has_value())
                {
                    // TODO: Implement range mapping that is provided by caller
                    double mapped_value = value.value();
                    current_state.add_input(range, mapped_value);
                }
            });
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    input_tag_collector input_context<actions_t, states_t, ranges_t>::build_tag_collector(
        input_state<actions_t, states_t, ranges_t>& current_state) const
    {
        return [this, &current_state](const details::input_wrapper& raw_input)
        {
            const auto& it = std::find(index_map.cbegin(), index_map.cend(), raw_input);
            if (it == index_map.cend())
            {
                return false;
            }

            std::size_t index = std::distance(index_map.cbegin(), it);
            collector_map[index](current_state, std::nullopt);
            return true;
        };
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    input_value_collector input_context<actions_t, states_t, ranges_t>::build_value_collector(
        input_state<actions_t, states_t, ranges_t>& current_state) const
    {
        return [this, &current_state](const details::input_wrapper& raw_input, const double value)
        {
            const auto& it = std::find(index_map.cbegin(), index_map.cend(), raw_input);
            if (it == index_map.cend())
            {
                return false;
            }

            std::size_t index = std::distance(index_map.cbegin(), it);
            collector_map[index](current_state, std::make_optional(value));
            return true;
        };
    }
} // namespace input

#endif
