#include "avis/middleware/input/details/input_wrapper.h"

#include "avis/core/common.h"

namespace input::details
{
    bool input_wrapper::operator==(const input_wrapper& other) const
    {
        return (input_type_id == other.input_type_id) &&
               (input_type_size == other.input_type_size) &&
               (std::memcmp(input_type_value.data(), other.input_type_value.data(), input_type_size) == 0);
    }
} // namespace input::details
