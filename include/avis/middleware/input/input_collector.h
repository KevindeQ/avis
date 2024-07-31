#ifndef MIDDLEWARE_INPUT_INPUT_COLLECTOR_H
#define MIDDLEWARE_INPUT_INPUT_COLLECTOR_H

#include "avis/core/common.h"
#include "avis/middleware/input/details/input_wrapper.h"
#include "avis/middleware/input/input_context.h"
#include "avis/middleware/input/input_state.h"

namespace input
{
    template<typename raw_input_t>
    using input_tag_collector = std::function<void(raw_input_t raw_input)>;

    class input_collector
    {
    public:
        template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
        input_collector(
            input_state<actions_t, states_t, ranges_t>& current_state,
            std::vector<input_context<actions_t, states_t, ranges_t>>& contexts);

        template<typename raw_input_t>
        input_tag_collector<raw_input_t> build_tag_collector();

    private:
        std::function<void(const details::input_wrapper& input_value)> generic_value_collector;
    };

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    input_collector::input_collector(
        input_state<actions_t, states_t, ranges_t>& current_state,
        std::vector<input_context<actions_t, states_t, ranges_t>>& contexts) :
        generic_value_collector{}
    {
        generic_value_collector = [&contexts, &current_state](const details::input_wrapper& input_value)
        {
            for (const input_context<actions_t, states_t, ranges_t>& context : std::ranges::reverse_view{ contexts })
            {
                input_context_collector collector = context.build_context_collector(current_state);

                bool value_mapped = collector(input_value);
                if (value_mapped)
                {
                    // Only allow the first context (from the top of the stack) to a map value. Other contexts that
                    // might want to map the same value are not called.
                    return;
                }
            }
        };
    }

    template<typename raw_input_t>
    input_tag_collector<raw_input_t> input_collector::build_tag_collector()
    {
        return [this](raw_input_t raw_input)
        {
            details::input_wrapper value_wrapper{ raw_input };
            generic_value_collector(value_wrapper);
        };
    }
} // namespace input

#endif
