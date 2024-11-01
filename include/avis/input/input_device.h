#ifndef INPUT_INPUT_DEVICE_H
#define INPUT_INPUT_DEVICE_H

#include "avis/common.h"
#include "avis/input/input_collector.h"

namespace input
{
    class input_device
    {
    public:
        virtual void decode_input(const RAWINPUT* raw_message, input_collector& context) = 0;
    };
} // namespace input

#endif
