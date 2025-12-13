#include "avis/data/serialization/memory_view_reader.h"

namespace data
{
    memory_view_reader::memory_view_reader(std::span<unsigned char> data) : _data{ data }, _current_offset{ 0 } {}

    void memory_view_reader::field(std::int32_t& value)
    {
        read_integral_type(value);
    }

    void memory_view_reader::field(std::int16_t& value)
    {
        read_integral_type(value);
    }

    void memory_view_reader::field(std::int8_t& value)
    {
        read_integral_type(value);
    }

    void memory_view_reader::field(std::uint32_t& value)
    {
        read_integral_type(value);
    }

    void memory_view_reader::field(std::uint16_t& value)
    {
        read_integral_type(value);
    }

    void memory_view_reader::field(std::uint8_t& value)
    {
        read_integral_type(value);
    }

    void memory_view_reader::field(float& value)
    {
        read_integral_type(value);
    }

    void memory_view_reader::field(double& value)
    {
        read_integral_type(value);
    }
}