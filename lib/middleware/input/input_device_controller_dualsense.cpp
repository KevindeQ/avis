#include "avis/middleware/input/input_device_controller_dualsense.h"

// hidsdi.h must be included before hidpi because some types defined in hidsdi.h are required in hidpi.h
// clang-format off
#include <hidsdi.h>
#include <hidpi.h>
// clang-format on

namespace input
{
    input_device_controller_dualsense::input_device_controller_dualsense(HWND window_handle) :
        buffer_preparsed_data{},
        buffer_caps{},
        buffer_button_caps{}
    {
        static std::once_flag registered{};
        std::call_once(
            registered,
            [this, window_handle]()
            {
                RAWINPUTDEVICE input_device_descriptor{};
                input_device_descriptor.usUsagePage = 0x0001;
                input_device_descriptor.usUsage = 0x0005;
                input_device_descriptor.dwFlags = 0;
                input_device_descriptor.hwndTarget = window_handle;

                if (RegisterRawInputDevices(&input_device_descriptor, 1, sizeof(input_device_descriptor)) == FALSE)
                {
                    std::uint32_t error_value = GetLastError();
                    std::error_code error_code(error_value, std::system_category());
                    throw std::system_error(error_code, "Exception occurred");
                }
            });
    }

    void input_device_controller_dualsense::decode_input(const RAWINPUT* raw_message, input_collector& collector)
    {
        if (raw_message->header.dwType != RIM_TYPEHID)
        {
            return;
        }

        RID_DEVICE_INFO device_info{};
        device_info.cbSize = sizeof(RID_DEVICE_INFO);
        std::uint32_t buffer_size = device_info.cbSize;

        // Get more detailed device info. This class deals only with PS5 dualsense controllers. So, any messages that
        // come from different devices need to be filtered out. Get more detailed device information so the vendor id
        // and product id can be compared to known values.
        std::uint32_t error_code =
            GetRawInputDeviceInfoW(raw_message->header.hDevice, RIDI_DEVICEINFO, &device_info, &buffer_size);
        if (error_code < 0)
        {
            return;
        }
        if ((device_info.hid.dwVendorId != vendor_id_sony) || (device_info.hid.dwProductId != product_id_controller))
        {
            return;
        }

        buffer_size = 0;
        error_code = GetRawInputDeviceInfoW(raw_message->header.hDevice, RIDI_PREPARSEDDATA, 0, &buffer_size);
        if (error_code < 0)
        {
            return;
        }

        buffer_preparsed_data.resize(buffer_size);
        error_code = GetRawInputDeviceInfoW(
            raw_message->header.hDevice, RIDI_PREPARSEDDATA, buffer_preparsed_data.data(), &buffer_size);
        if (error_code < 0)
        {
            return;
        }

        /*std::wstring raw_text = L"";
        for (const std::byte& raw_byte : buffer_preparsed_data | std::views::drop(64) | std::views::take(16))
        {
            raw_text += std::format(L"{:x} ", static_cast<unsigned char>(raw_byte));
        }
        OutputDebugString(std::format(L"Raw: {}\n", raw_text).c_str());*/

        PHIDP_PREPARSED_DATA preparsed_data = reinterpret_cast<PHIDP_PREPARSED_DATA>(buffer_preparsed_data.data());
        HIDP_CAPS caps;
        std::int32_t data_valid = HidP_GetCaps(preparsed_data, &caps);
        if (data_valid != HIDP_STATUS_SUCCESS)
        {
            return;
        }

        buffer_caps.resize(caps.NumberInputValueCaps * sizeof(HIDP_VALUE_CAPS));
        PHIDP_VALUE_CAPS value_caps = reinterpret_cast<PHIDP_VALUE_CAPS>(buffer_caps.data());
        data_valid = HidP_GetValueCaps(HidP_Input, value_caps, &caps.NumberInputValueCaps, preparsed_data);
        if (data_valid != HIDP_STATUS_SUCCESS)
        {
            return;
        }

        /*std::wstring usage_text = L"";*/
        for (std::uint16_t index = 0; index < caps.NumberInputValueCaps; ++index)
        {
            unsigned long value = 0;
            data_valid = HidP_GetUsageValue(
                HidP_Input,
                value_caps[index].UsagePage,
                0,
                value_caps[index].Range.UsageMin,
                &value,
                preparsed_data,
                reinterpret_cast<PCHAR>(const_cast<RAWINPUT*>(raw_message)->data.hid.bRawData),
                raw_message->data.hid.dwSizeHid);
            if (data_valid != HIDP_STATUS_SUCCESS)
            {
                continue;
            }

            // TODO: Do something with the usage value
            /*usage_text += std::format(L"{} ", value);*/

            if (index < axis_value_map.size())
            {
                controller_axis axis = axis_value_map[index];
                collector.collect<controller_axis>(axis, value);
            }
        }
        /*OutputDebugStringW(std::format(L"Usage: {}\n", usage_text).c_str());*/

        buffer_button_caps.resize(caps.NumberInputButtonCaps * sizeof(HIDP_BUTTON_CAPS));
        PHIDP_BUTTON_CAPS button_caps = reinterpret_cast<PHIDP_BUTTON_CAPS>(buffer_button_caps.data());
        data_valid = HidP_GetButtonCaps(HidP_Input, button_caps, &caps.NumberInputButtonCaps, preparsed_data);
        if (data_valid != HIDP_STATUS_SUCCESS)
        {
            return;
        }

        for (std::uint16_t index = 0; index < caps.NumberInputButtonCaps; ++index)
        {
            unsigned long usage_count = button_caps->Range.UsageMax - button_caps->Range.UsageMin + 1;
            std::vector<std::byte> buffer_usages{};
            buffer_usages.resize(usage_count * sizeof(USAGE));
            USAGE* usages = reinterpret_cast<USAGE*>(buffer_usages.data());
            data_valid = HidP_GetUsages(
                HidP_Input,
                button_caps[index].UsagePage,
                0,
                usages,
                &usage_count,
                preparsed_data,
                reinterpret_cast<PCHAR>(const_cast<RAWINPUT*>(raw_message)->data.hid.bRawData),
                raw_message->data.hid.dwSizeHid);
            if (data_valid != HIDP_STATUS_SUCCESS)
            {
                continue;
            }

            // This loop only works because the usages have been determined to only contain values when they contain
            // information about actual buttons. Besides, this doesn't need to be flexible because there is no variation
            // in hardware and only needs to work for one specific device.
            for (std::uint32_t usage_index = 0; usage_index < usage_count; ++usage_index)
            {
                controller_buttons button = button_value_map[usages[usage_index] - 1];
                collector.collect<controller_buttons>(button);
            }
        }
    }
} // namespace input
