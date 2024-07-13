#include "avis/middleware/input/details/input_wrapper.h"

#include "avis/core/common.h"

namespace input::details
{
    bool input_wrapper::operator==(const input_wrapper& other) const
    {
        return (type_id == other.type_id) &&
               std::memcmp(input_value.data(), other.input_value.data(), input_value.size());
    }
} // namespace input::details
