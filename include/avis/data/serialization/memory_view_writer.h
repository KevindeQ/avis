#ifndef DATA_SERIALIZATION_MEMORY_VIEW_WRITER_H
#define DATA_SERIALIZATION_MEMORY_VIEW_WRITER_H

#include "avis/common.h"

namespace data
{
    class mmory_view_writer
    {
    public:
        mmory_view_writer(std::span<unsigned char> data);

        void field(const std::int32_t value);
        void field(const std::int16_t value);
        void field(const std::int8_t value);

        void field(const std::uint32_t value);
        void field(const std::uint16_t value);
        void field(const std::uint8_t value);

        void field(const float value);
        void field(const double value);

    private:
        template<typename integral_type>
        void write_integral_type(const integral_type value);

        std::span<unsigned char> _data;
        std::size_t _current_offset;
    };

    template<typename integral_type>
    void mmory_view_writer::write_integral_type(const integral_type value)
    {
        // TODO: Maybe get naked type for value (instead of reference type)?
        std::size_t byte_size = sizeof(value);
        if (_data.size() <= _current_offset + byte_size)
        {
            throw std::runtime_error{ "Not enough data" };
        }

        std::copy_n(reinterpret_cast<const unsigned char*>(&value), byte_size, _data.data() + _current_offset);

        _current_offset += byte_size;
    }
} // namespace data

#endif
