#include "avis/middleware/input/input_device_mouse.h"

namespace input
{
    input_device_mouse::input_device_mouse(HWND window_handle)
    {
        static std::once_flag registered{};
        std::call_once(
            registered,
            [this, window_handle]()
            {
                RAWINPUTDEVICE input_device_descriptor{};
                input_device_descriptor.usUsagePage = 0x0001;
                input_device_descriptor.usUsage = 0x0001;
                input_device_descriptor.dwFlags = RIDEV_NOLEGACY;
                input_device_descriptor.hwndTarget = window_handle;

                RegisterRawInputDevices(&input_device_descriptor, 1, sizeof(input_device_descriptor));
            });
    }

    std::vector<details::input_wrapper> input_device_mouse::decode_input(const RAWINPUT* raw_message)
    {
        if (raw_message->header.dwType != RIM_TYPEMOUSE)
        {
            return {};
        }

        std::vector<details::input_wrapper> raw_inputs{};

        const RAWMOUSE& mouse_message = raw_message->data.mouse;
        decode_mouse_position(mouse_message, raw_inputs);
        decode_mouse_buttons(mouse_message, raw_inputs);
        decode_mouse_wheel(mouse_message, raw_inputs);

        return raw_inputs;
    }

    void input_device_mouse::decode_mouse_position(
        const RAWMOUSE& mouse_message, std::vector<details::input_wrapper>& raw_inputs)
    {
        if (mouse_message.usFlags & MOUSE_MOVE_ABSOLUTE)
        {
            RECT rect{};
            if (mouse_message.usFlags & MOUSE_VIRTUAL_DESKTOP)
            {
                rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
                rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
                rect.right = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                rect.bottom = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            }
            else
            {
                rect.left = 0;
                rect.top = 0;
                rect.right = GetSystemMetrics(SM_CXSCREEN);
                rect.bottom = GetSystemMetrics(SM_CYSCREEN);
            }

            std::int32_t absolute_x =
                (static_cast<std::int64_t>(mouse_message.lLastX) * static_cast<std::int64_t>(rect.right) /
                 std::numeric_limits<std::uint16_t>::max()) +
                rect.left;
            std::int32_t absolute_y =
                (static_cast<std::int64_t>(mouse_message.lLastY) * static_cast<std::int64_t>(rect.bottom) /
                 std::numeric_limits<std::uint16_t>::max()) +
                rect.top;

            // TODO: Save absolute position
        }
        else if (mouse_message.lLastX != 0 || mouse_message.lLastY != 0)
        {
            std::int32_t relative_x = mouse_message.lLastX;
            std::int32_t relative_y = mouse_message.lLastY;

            // TODO: Save relative position
        }
    }

    void input_device_mouse::decode_mouse_buttons(
        const RAWMOUSE& mouse_message, std::vector<details::input_wrapper>& raw_inputs)
    {
        if (mouse_message.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
        {
            // TODO: Save left button up
        }
        if (mouse_message.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
        {
            // TODO: Save left button up
        }

        if (mouse_message.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
        {
            // TODO: Save right button up
        }
        if (mouse_message.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
        {
            // TODO: Save right button up
        }

        if (mouse_message.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
        {
            // TODO: Save middle button up
        }
        if (mouse_message.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
        {
            // TODO: Save middle button up
        }
    }

    void input_device_mouse::decode_mouse_wheel(
        const RAWMOUSE& mouse_message, std::vector<details::input_wrapper>& raw_inputs)
    {
        if ((mouse_message.usButtonFlags & ~RI_MOUSE_WHEEL) && (mouse_message.usButtonFlags & ~RI_MOUSE_HWHEEL))
        {
            // No difference in mouse wheel position was found so bail out
            return;
        }

        std::uint16_t wheel_delta = static_cast<std::uint16_t>(mouse_message.usButtonData);
        float scroll_delta = static_cast<float>(wheel_delta) / WHEEL_DELTA;

        if (mouse_message.usButtonFlags & RI_MOUSE_WHEEL)
        {
            std::uint32_t scroll_chars = 1;
            SystemParametersInfo(SPI_GETWHEELSCROLLCHARS, 0, &scroll_chars, 0);
            scroll_delta *= scroll_chars;

            // TODO: Save horizontal wheel delta
        }

        if (mouse_message.usButtonFlags & RI_MOUSE_HWHEEL)
        {
            std::uint32_t scroll_lines = 1;
            SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &scroll_lines, 0);
            if (scroll_lines != WHEEL_PAGESCROLL)
            {
                scroll_delta *= scroll_lines;
            }

            // TODO: Save vertical wheel delta
        }
    }
} // namespace input
