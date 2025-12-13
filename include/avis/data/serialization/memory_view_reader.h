#ifndef DATA_SERIALIZATION_MEMORY_VIEW_READER_H
#define DATA_SERIALIZATION_MEMORY_VIEW_READER_H

#include "avis/common.h"

namespace data
{
    class memory_view_reader
    {
    public:
        memory_view_reader(std::span<unsigned char> data);

        void field(std::int32_t& value);
        void field(std::int16_t& value);
        void field(std::int8_t& value);

        void field(std::uint32_t& value);
        void field(std::uint16_t& value);
        void field(std::uint8_t& value);

        void field(float& value);
        void field(double& value);

    private:
        template<typename integral_type>
        void read_integral_type(integral_type& value);

        std::span<unsigned char> _data;
        std::size_t _current_offset;
    };

    template<typename integral_type>
    void memory_view_reader::read_integral_type(integral_type& value)
    {
        // TODO: Maybe get naked type for value (instead of reference type)?
        std::size_t byte_size = sizeof(value);
        if (_data.size() <= _current_offset + byte_size)
        {
            throw std::runtime_error{ "Not enough data available" };
        }

        std::copy_n(_data.data() + _current_offset, byte_size, reinterpret_cast<unsigned char*>(&value));

        _current_offset += byte_size;
    }
} // namespace data

#endif
