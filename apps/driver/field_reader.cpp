#include "field_reader.h"

field_reader::field_reader(std::span<unsigned char> data) : data_{ std::as_writable_bytes(data) } {}

field_reader::field_reader(std::span<char> data) : data_{ std::as_writable_bytes(data) } {}

field_reader::field_reader(std::span<std::byte> data) : data_{ data } {}

void field_reader::field(std::int64_t& value)
{
    read_integral_type(value);
}

void field_reader::field(std::int32_t& value)
{
    read_integral_type(value);
}

void field_reader::field(std::int16_t& value)
{
    read_integral_type(value);
}

void field_reader::field(std::int8_t& value)
{
    read_integral_type(value);
}

void field_reader::field(std::uint64_t& value)
{
    read_integral_type(value);
}

void field_reader::field(std::uint32_t& value)
{
    read_integral_type(value);
}

void field_reader::field(std::uint16_t& value)
{
    read_integral_type(value);
}

void field_reader::field(std::uint8_t& value)
{
    read_integral_type(value);
}

void field_reader::field(float& value)
{
    read_integral_type(value);
}

void field_reader::field(double& value)
{
    read_integral_type(value);
}
