#ifndef MIDDLEWARE_INPUT_INPUT_DECODER_H
#define MIDDLEWARE_INPUT_INPUT_DECODER_H

#include "avis/core/common.h"
#include "avis/middleware/input/input_context.h"
#include "avis/middleware/input/input_device.h"
#include "avis/middleware/input/input_state.h"

namespace input
{
    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    class input_decoder
    {
    public:
        template<
            typename device_type,
            typename... arguments_t,
            std::enable_if_t<std::is_base_of_v<input_device, device_type>>* = nullptr>
        void register_device(arguments_t&&... arguments);

        void decode(const WPARAM wparam, const LPARAM lparam);

        input_state<actions_t, states_t, ranges_t> snapshot_inputs();
        void clear_inputs();

        void push_context(input_context<actions_t, states_t, ranges_t>& context);
        void pop_context();

    private:
        void map_value(const details::input_wrapper& raw_input);

        std::vector<std::unique_ptr<input_device>> devices;
        std::vector<input_context<actions_t, states_t, ranges_t>> contexts;

        input_state<actions_t, states_t, ranges_t> current_inputs;
    };

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    template<
        typename device_type,
        typename... arguments_t,
        std::enable_if_t<std::is_base_of_v<input_device, device_type>>*>
    void input_decoder<actions_t, states_t, ranges_t>::register_device(arguments_t&&... arguments)
    {
        devices.emplace_back(std::make_unique<device_type>(std::forward<arguments_t>(arguments)...));
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_decoder<actions_t, states_t, ranges_t>::decode(const WPARAM wparam, const LPARAM lparam)
    {
        constexpr size_t buffer_size = sizeof(RAWINPUT);
        std::array<std::byte, buffer_size> buffer{};

        std::uint32_t message_size = buffer_size;
        GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buffer.data(), &message_size, sizeof(RAWINPUTHEADER));

        RAWINPUT* raw_message = reinterpret_cast<RAWINPUT*>(buffer.data());
        for (std::unique_ptr<input_device>& device : devices)
        {
            std::vector<details::input_wrapper> inputs = device->decode_input(raw_message);

            for (const details::input_wrapper& raw_input : inputs)
            {
                map_value(raw_input);
            }
        }
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    input_state<actions_t, states_t, ranges_t> input_decoder<actions_t, states_t, ranges_t>::snapshot_inputs()
    {
        return input_state<actions_t, states_t, ranges_t>{ current_inputs };
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_decoder<actions_t, states_t, ranges_t>::clear_inputs()
    {
        current_inputs.clear();
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_decoder<actions_t, states_t, ranges_t>::push_context(input_context<actions_t, states_t, ranges_t>& context)
    {
        contexts.push_back(context);
    }
    
    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_decoder<actions_t, states_t, ranges_t>::pop_context()
    {
        contexts.pop_back();
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_decoder<actions_t, states_t, ranges_t>::map_value(const details::input_wrapper& raw_input)
    {
        for (const input_context<actions_t, states_t, ranges_t>& context : std::ranges::reverse_view{ contexts })
        {
            bool value_mapped = context.map_value(raw_input, current_inputs);
            if (value_mapped)
            {
                continue;
            }
        }
    }
} // namespace input

#endif
