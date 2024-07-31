#ifndef MIDDLEWARE_INPUT_INPUT_DEVICE_CONTROLLER_DUALSENSE_H
#define MIDDLEWARE_INPUT_INPUT_DEVICE_CONTROLLER_DUALSENSE_H

#include "avis/core/common.h"
#include "avis/middleware/input/input_context.h"
#include "avis/middleware/input/input_device.h"

namespace input
{
    enum class controller_axis
    {
        axis_R2 = 0,
        axis_L2 = 1,
        axis_right_stick_y = 2,
        axis_right_stick_x = 3,
        axis_left_stick_y = 4,
        axis_left_stick_x = 5
    };

    enum class controller_buttons
    {
        button_square = 1,
        button_cross = 2,
        button_circle = 3,
        button_triangle = 4,
        button_L1 = 5,
        button_R1 = 6,
        button_L2 = 7,
        button_R2 = 8,
        button_create = 9,
        button_options = 10,
        button_left_stick = 11,
        button_right_stick = 12,
        button_ps = 13,
        button_touch_pad = 14,
        button_mute = 15
    };

    class input_device_controller_dualsense : public input_device
    {
    public:
        input_device_controller_dualsense(HWND window_handle);

        void decode_input(const RAWINPUT* raw_message, input_collector& collector) override;

    private:
        static constexpr std::uint32_t vendor_id_sony = 0x054C;
        static constexpr std::uint32_t product_id_controller = 0x0CE6;

        // Do not change the order in this array. The index in the array corresponds with the usage value that is
        // reported by RAWINPUT.
        static constexpr std::array<controller_axis, 6> axis_value_map = { controller_axis::axis_R2,
                                                                           controller_axis::axis_L2,
                                                                           controller_axis::axis_right_stick_y,
                                                                           controller_axis::axis_right_stick_x,
                                                                           controller_axis::axis_left_stick_y,
                                                                           controller_axis::axis_left_stick_x };

        // Do not change the order in this array. The index in the array corresponds with the usage value that is
        // reported by RAWINPUT.
        static constexpr std::array<controller_buttons, 15> button_value_map = {
            controller_buttons::button_square,     controller_buttons::button_cross,
            controller_buttons::button_circle,     controller_buttons::button_triangle,
            controller_buttons::button_L1,         controller_buttons::button_R1,
            controller_buttons::button_L2,         controller_buttons::button_R2,
            controller_buttons::button_create,     controller_buttons::button_options,
            controller_buttons::button_left_stick, controller_buttons::button_right_stick,
            controller_buttons::button_ps,         controller_buttons::button_touch_pad,
            controller_buttons::button_mute
        };

        std::vector<std::byte> buffer_preparsed_data;
        std::vector<std::byte> buffer_caps;
        std::vector<std::byte> buffer_button_caps;
    };
} // namespace input

#endif
