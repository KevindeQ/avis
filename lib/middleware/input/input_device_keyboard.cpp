#include "avis/middleware/input/input_device_keyboard.h"

namespace input
{
    input_device_keyboard::input_device_keyboard(HWND window_handle)
    {
        static std::once_flag registered{};
        std::call_once(
            registered,
            [this, window_handle]()
            {
                RAWINPUTDEVICE input_device_descriptor{};
                input_device_descriptor.usUsagePage = 0x0001;
                input_device_descriptor.usUsage = 0x0006;
                input_device_descriptor.dwFlags = 0;
                input_device_descriptor.hwndTarget = window_handle;

                if (RegisterRawInputDevices(&input_device_descriptor, 1, sizeof(input_device_descriptor)) == FALSE)
                {
                    DWORD error_value = GetLastError();
                    std::error_code error_code(error_value, std::system_category());
                    throw std::system_error(error_code, "Exception occurred");
                }
            });
    }

    void input_device_keyboard::decode_input(const RAWINPUT* raw_message, input_collector& collector)
    {
        if (raw_message->header.dwType != RIM_TYPEKEYBOARD)
        {
            return;
        }

        const RAWKEYBOARD& keyboard_message = raw_message->data.keyboard;

        std::uint32_t virtual_key = keyboard_message.VKey;
        std::uint32_t scan_code = keyboard_message.MakeCode;
        std::uint32_t flags = keyboard_message.Flags;

        // Ignore key overrun state and keys not mapped to any virtual key code
        if (scan_code == KEYBOARD_OVERRUN_MAKE_CODE || virtual_key >= UCHAR_MAX)
        {
            return;
        }

        // Correct left-hand / right-hand shift
        if (virtual_key == VK_SHIFT)
        {
            virtual_key = MapVirtualKeyW(scan_code, MAPVK_VSC_TO_VK_EX);
        }

        // The scan code is currently not necessary. However, this is left here in case it is needed in the future.
        // else if (virtual_key == VK_NUMLOCK)
        //{
        //    // correct PAUSE/BREAK and NUM LOCK silliness, and set the extended bit
        //    scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC) | 0x100;
        //}

        // e0 and e1 are escape sequences used for certain special keys, such as PRINT and PAUSE/BREAK.
        // see http://www.win.tue.nl/~aeb/linux/kbd/scancodes-1.html
        const bool is_e0 = (flags & RI_KEY_E0) != 0;
        const bool is_e1 = (flags & RI_KEY_E1) != 0;

        // The scan code is currently not necessary. However, this is left here in case it is needed in the future.
        // if (is_e1)
        //{
        //    // for escaped sequences, turn the virtual key into the correct scan code using MapVirtualKey.
        //    // However, MapVirtualKey is unable to map VK_PAUSE (this is a known bug), hence we map that by hand.
        //    if (virtual_key == VK_PAUSE)
        //    {
        //        scan_code = 0x45;
        //    }
        //    else
        //    {
        //        scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
        //    }
        //}

        key_code mapped_key_code = map_virtual_key(virtual_key, is_e0);

        // A key can either produce a "make" or "break" flag. This is used to differentiate between keypresses and
        // releases. See http://www.win.tue.nl/~aeb/linux/kbd/scancodes-1.html.
        const bool key_up = (flags & RI_KEY_BREAK) != 0;

        collector.collect(mapped_key_code);
    }

    key_code input_device_keyboard::map_virtual_key(const std::uint32_t virtual_key, const bool is_e0) const
    {
        switch (virtual_key)
        {
        case VK_CONTROL:
            // Right-hand CONTROL and ALT have their e0 bit set
            if (is_e0)
            {
                return key_code::key_right_control;
            }
            else
            {
                return key_code::key_left_control;
            }
            break;

        case VK_MENU:
            if (is_e0)
            {
                return key_code::key_right_alt;
            }
            else
            {
                return key_code::key_left_alt;
            }
            break;

        case VK_RETURN:
            // NUMPAD ENTER has its e0 bit set
            if (is_e0)
            {
                return key_code::key_numpad_enter;
            }
            break;

        case VK_INSERT:
            // The standard INSERT, DELETE, HOME, END, PRIOR and NEXT keys will always have their e0 bit set, but the
            // corresponding keys on the NUMPAD will not.
            if (!is_e0)
            {
                return key_code::key_numpad_0;
            }
            break;

        case VK_DELETE:
            if (!is_e0)
            {
                return key_code::key_numpad_decimal;
            }
            break;

        case VK_HOME:
            if (!is_e0)
            {
                return key_code::key_numpad_7;
            }
            break;

        case VK_END:
            if (!is_e0)
            {
                return key_code::key_numpad_1;
            }
            break;

        case VK_PRIOR:
            if (!is_e0)
            {
                return key_code::key_numpad_9;
            }
            break;

        case VK_NEXT:
            if (!is_e0)
            {
                return key_code::key_numpad_3;
            }
            break;

        case VK_LEFT:
            // The standard arrow keys will always have their e0 bit set, but the corresponding keys on the NUMPAD
            // will not.
            if (!is_e0)
            {
                return key_code::key_numpad_4;
            }
            break;

        case VK_RIGHT:
            if (!is_e0)
            {
                return key_code::key_numpad_6;
            }
            break;

        case VK_UP:
            if (!is_e0)
            {
                return key_code::key_numpad_8;
            }
            break;

        case VK_DOWN:
            if (!is_e0)
            {
                return key_code::key_numpad_2;
            }
            break;

        case VK_CLEAR:
            // NUMPAD 5 doesn't have its e0 bit set
            if (!is_e0)
            {
                return key_code::key_numpad_5;
            }
            break;
        }

        return static_cast<key_code>(virtual_key);
    }
} // namespace input
