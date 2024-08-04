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

        void decode(
            input_state<actions_t, states_t, ranges_t>& current_inputs, const WPARAM wparam, const LPARAM lparam);

        void push_context(input_context<actions_t, states_t, ranges_t>& context);
        void pop_context();

    private:
        void map_value(const details::input_wrapper& raw_input);

        std::vector<std::unique_ptr<input_device>> devices;
        std::vector<input_context<actions_t, states_t, ranges_t>> contexts;
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
    void input_decoder<actions_t, states_t, ranges_t>::decode(
        input_state<actions_t, states_t, ranges_t>& current_inputs, const WPARAM wparam, const LPARAM lparam)
    {
        std::uint32_t buffer_size = 0;
        std::uint32_t error_code = GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &buffer_size, sizeof(RAWINPUTHEADER));
        if (error_code == 0xFFFFFFFF)
        {
            std::uint32_t error_value = GetLastError();
            std::error_code error_code(error_value, std::system_category());
            throw std::system_error(error_code, "Exception occurred");
        }

        std::vector<std::byte> buffer{};
        buffer.resize(buffer_size);
        error_code = GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buffer.data(), &buffer_size, sizeof(RAWINPUTHEADER));
        if (error_code == 0xFFFFFFFF)
        {
            std::uint32_t error_value = GetLastError();
            std::error_code error_code(error_value, std::system_category());
            throw std::system_error(error_code, "Exception occurred");
        }

        input_collector collector{ current_inputs, contexts };

        RAWINPUT* raw_message = reinterpret_cast<RAWINPUT*>(buffer.data());
        for (std::unique_ptr<input_device>& device : devices)
        {
            device->decode_input(raw_message, collector);
        }
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_decoder<actions_t, states_t, ranges_t>::push_context(
        input_context<actions_t, states_t, ranges_t>& context)
    {
        contexts.push_back(context);
    }

    template<enumeration actions_t, enumeration states_t, enumeration ranges_t>
    void input_decoder<actions_t, states_t, ranges_t>::pop_context()
    {
        contexts.pop_back();
    }
} // namespace input

#endif
