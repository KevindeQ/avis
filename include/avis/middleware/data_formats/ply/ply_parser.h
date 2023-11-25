#ifndef MIDDLEWARE_DATA_FORMATS_PLY_PLY_PARSER_H
#define MIDDLEWARE_DATA_FORMATS_PLY_PLY_PARSER_H

#include "avis/core/common.h"
#include "avis/core/streams/memory_stream.h"
#include "avis/middleware/geometry/data_store.h"

namespace data_formats::ply
{
    enum class ply_data_format
    {
        unknown,

        ascii_1_0,
        binary_little_endian,
        binary_big_endian
    };

    class ply_parser
    {
    public:
        // The implementation made use of the format description at the following website:
        // https://paulbourke.net/dataformats/ply/.
        geometry::data_store parse(const streams::memory_stream& contents) const;

    private:
        struct block_mapping
        {
        public:
            std::string_view name;
            std::size_t element_count;
            std::size_t block_index;
        };

        bool parse_data_format(const std::vector<std::string_view> parts, ply_data_format& data_format) const;
        bool parse_element(const std::vector<std::string_view> parts, block_mapping& mapping) const;
        bool parse_property(
            const std::vector<std::string_view> parts, geometry::data_layout& current_block_layout) const;

        geometry::data_element_format convert_from_string(const std::string_view datatype) const;

        std::size_t format_byte_count(const geometry::data_element_format format) const;
    };
} // namespace data_formats::ply

#endif
