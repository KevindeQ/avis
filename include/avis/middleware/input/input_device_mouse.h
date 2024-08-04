#ifndef MIDDLEWARE_INPUT_INPUT_DEVICE_MOUSE_H
#define MIDDLEWARE_INPUT_INPUT_DEVICE_MOUSE_H

#include "avis/core/common.h"
#include "avis/middleware/input/input_context.h"
#include "avis/middleware/input/input_device.h"

namespace input
{
    class input_device_mouse : public input_device
    {
    public:
        input_device_mouse(HWND window_handle);

        void decode_input(const RAWINPUT* raw_message, input_collector& collector) override;

    private:
        void decode_mouse_position(const RAWMOUSE& mouse_message, input_collector& collector);
        void decode_mouse_buttons(const RAWMOUSE& mouse_message, input_collector& collector);
        void decode_mouse_wheel(const RAWMOUSE& mouse_message, input_collector& collector);
    };
} // namespace input

#endif
