#include "avis/data/serialization/memory_view_writer.h"

namespace data
{
    mmory_view_writer::mmory_view_writer(std::span<unsigned char> data) : _data{ data }, _current_offset{ 0 } {}

    void mmory_view_writer::field(const std::int32_t value)
    {
        write_integral_type(value);
    }

    void mmory_view_writer::field(const std::int16_t value)
    {
        write_integral_type(value);
    }

    void mmory_view_writer::field(const std::int8_t value)
    {
        write_integral_type(value);
    }

    void mmory_view_writer::field(const std::uint32_t value)
    {
        write_integral_type(value);
    }

    void mmory_view_writer::field(const std::uint16_t value)
    {
        write_integral_type(value);
    }

    void mmory_view_writer::field(const std::uint8_t value)
    {
        write_integral_type(value);
    }

    void mmory_view_writer::field(const float value)
    {
        write_integral_type(value);
    }

    void mmory_view_writer::field(const double value)
    {
        write_integral_type(value);
    }
}
