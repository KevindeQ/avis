#ifndef RUNTIME_DATA_FIELD_EXTENSIONS_H
#define RUNTIME_DATA_FIELD_EXTENSIONS_H

#include "Eigen/Geometry"
#include "avis/runtime/data/field.h"

namespace data
{
    template<>
    class field_reader<Eigen::Vector3f> : public basic_field_reader
    {
    public:
        static Eigen::Vector3f read(page_view& data, const std::size_t offset_in_bytes);
    };

    inline Eigen::Vector3f field_reader<Eigen::Vector3f>::read(page_view& data, const std::size_t offset_in_bytes)
    {
        std::optional<float> x = read_value<float>(data, offset_in_bytes);
        if (!x.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Unable to read field value." };
        }

        std::optional<float> y = read_value<float>(data, offset_in_bytes);
        if (!y.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Unable to read field value." };
        }

        std::optional<float> z = read_value<float>(data, offset_in_bytes);
        if (!z.has_value())
        {
            throw std::runtime_error{ "Invalid file format. Unable to read field value." };
        }

        return Eigen::Vector3f{ x.value(), y.value(), z.value() };
    }
} // namespace data

#endif
