#ifndef MIDDLEWARE_GEOMETRY_DATA_LAYOUT_H
#define MIDDLEWARE_GEOMETRY_DATA_LAYOUT_H

#include "avis/core/common.h"

namespace geometry
{
    enum class data_element_format : std::uint8_t
    {
        unknown,

        signed_char,
        unsigned_char,
        signed_short,
        unsigned_short,
        signed_int,
        unsigned_int,
        single_float,
        double_float
    };

    struct data_element_descriptor
    {
    public:
        std::string name;
        std::size_t slot;
        data_element_format format;

        // General data placement attributes. These properties are necessary to keep data alignment in mind whereby the
        // elements don't exactly fit their field but have also padding bytes included..
        std::size_t byte_offset;
        std::size_t byte_count;
        std::size_t byte_stride;
    };

    class data_layout
    {
    public:
        void add_element(const data_element_descriptor& descriptor);
        void add_element_back(
            const std::string_view name,
            const data_element_format format,
            const std::size_t byte_count,
            const std::size_t byte_stride);

        void reset();

        std::size_t stride() const;

        std::size_t offset_by_id(const std::size_t id) const;

    private:
        std::size_t sum_element_stride() const;

        // Descriptors are sorted in ascending order on data_element_descriptor::slot
        std::vector<data_element_descriptor> descriptors;
    };
} // namespace geometry

#endif
