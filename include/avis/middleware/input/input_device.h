#ifndef MIDDLEWARE_INPUT_INPUT_DEVICE_H
#define MIDDLEWARE_INPUT_INPUT_DEVICE_H

#include "avis/core/common.h"
#include "avis/middleware/input/details/input_wrapper.h"

namespace input
{
    class input_device
    {
    public:
        virtual std::vector<details::input_wrapper> decode_input(const RAWINPUT* raw_message) = 0;
    };
}

#endif
