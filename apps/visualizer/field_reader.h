#ifndef VISUALIZER_FIELD_READER_H
#define VISUALIZER_FIELD_READER_H

#include <span>
#include <stdexcept>

class field_reader
{
public:
    field_reader(std::span<unsigned char> data);
    field_reader(std::span<char> data);
    field_reader(std::span<std::byte> data);

    void field(std::int64_t& value);
    void field(std::int32_t& value);
    void field(std::int16_t& value);
    void field(std::int8_t& value);

    void field(std::uint64_t& value);
    void field(std::uint32_t& value);
    void field(std::uint16_t& value);
    void field(std::uint8_t& value);

    void field(float& value);
    void field(double& value);

private:
    template<typename integral_type>
    void read_integral_type(integral_type& value)
    {
        const auto byte_size = sizeof(value);
        if (current_offset_ + byte_size >= data_.size())
        {
            throw std::out_of_range{ "Not enough data available" };
        }

        std::memcpy(&value, data_.data() + current_offset_, byte_size);
    }

    std::span<std::byte> data_;
    std::size_t current_offset_;
};

#endif