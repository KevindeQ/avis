#ifndef MIDDLEWARE_INPUT_INPUT_DEVICE_MOUSE_H
#define MIDDLEWARE_INPUT_INPUT_DEVICE_MOUSE_H

#include "avis/core/common.h"
#include "avis/middleware/input/input_device.h"

namespace input
{
    class input_device_mouse : public input_device
    {
    public:
        input_device_mouse(HWND window_handle);

        std::vector<details::input_wrapper> decode_input(const RAWINPUT* raw_message) override;

    private:
        void decode_mouse_position(const RAWMOUSE& mouse_message, std::vector<details::input_wrapper>& raw_inputs);
        void decode_mouse_buttons(const RAWMOUSE& mouse_message, std::vector<details::input_wrapper>& raw_inputs);
        void decode_mouse_wheel(const RAWMOUSE& mouse_message, std::vector<details::input_wrapper>& raw_inputs);
    };
} // namespace input

#endif
