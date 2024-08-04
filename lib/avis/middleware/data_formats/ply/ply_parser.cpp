#include "avis/middleware/data_formats/ply/ply_parser.h"

namespace data_formats::ply
{
    geometry::data_store ply_parser::parse(const streams::memory_stream& contents) const
    {
        using buffer_type = std::span<unsigned char>;
        buffer_type data = contents.data();

        bool valid_ply_file = false;
        ply_data_format data_format = ply_data_format::unknown;
        std::size_t data_offset = 0;

        std::vector<block_mapping> block_mappings{};

        geometry::data_store new_data_store{};
        geometry::data_layout current_block_layout{};
        block_mapping current_block_mapping{};
        bool in_block = false;

        // Parse the text header
        typename buffer_type::iterator it = data.begin();
        typename buffer_type::iterator end = data.end();

        typename buffer_type::iterator line_start_it = it;
        while (it != end)
        {
            if (*it == '\n')
            {
                std::string line{ line_start_it, it };
                trim(line);

                std::vector<std::string_view> parts = split(line, " ");

                // Check if the line was empty
                if (parts.size() <= 0)
                {
                    ++it;
                    continue;
                }

                // Parse the different line types
                if (parts[0] == "ply")
                {
                    valid_ply_file = true;
                }
                else if (parts[0] == "format")
                {
                    valid_ply_file = parse_data_format(parts, data_format);
                    if (!valid_ply_file)
                    {
                        break;
                    }
                }
                else if (parts[0] == "comment")
                {
                    // Skip comment lines. This block was intentionally left empty.
                }
                else if (parts[0] == "element")
                {
                    if (in_block)
                    {
                        std::size_t block_index = new_data_store.reserve_block(current_block_layout);
                        current_block_layout.reset();

                        current_block_mapping.block_index = block_index;
                        block_mappings.push_back(current_block_mapping);
                        in_block = false;
                    }

                    valid_ply_file = parse_element(parts, current_block_mapping);
                    if (!valid_ply_file)
                    {
                        break;
                    }

                    std::vector<block_mapping>::const_iterator it = std::find_if(
                        block_mappings.cbegin(),
                        block_mappings.cend(),
                        [&section_name = current_block_mapping.name](const block_mapping& mapping)
                        { return mapping.name == section_name; });
                    if (it != block_mappings.cend())
                    {
                        // More than one section with the same name exists. This is isn't allowed because it's
                        // impossible to know which section is refered to in further processing.
                        break;
                    }

                    in_block = true;
                }
                else if (parts[0] == "property")
                {
                    if (!in_block)
                    {
                        // Top-level block are not supported. So, a element must have been encountered before the
                        // current property.
                        valid_ply_file = false;
                        break;
                    }

                    valid_ply_file = parse_property(parts, current_block_layout);
                    if (!valid_ply_file)
                    {
                        break;
                    }
                }
                else if (parts[0] == "end_header")
                {
                    // Don't forget to include the last parsed block
                    if (in_block)
                    {
                        std::size_t block_index = new_data_store.reserve_block(current_block_layout);
                        current_block_layout.reset();

                        current_block_mapping.block_index = block_index;
                        block_mappings.push_back(current_block_mapping);
                        in_block = false;
                    }

                    // Finished parsing the header. Store the data offset for the data parsing. Include the '\n' in the
                    // data offset.
                    data_offset = std::distance(data.begin(), it) + 1;
                    break;
                }

                // Keep track of the new line start
                line_start_it = it + 1;
            }

            ++it;
        }

        if (!valid_ply_file)
        {
            throw std::runtime_error{ "Invalid ply file" };
        }

        // Parse the data. This implementation assumes that block_mappings is ordered such that the order for
        // block_mappings match the order the actual data is in.
        for (const block_mapping& mapping : block_mappings)
        {
            const geometry::data_layout& block_layout = new_data_store.block_layout(mapping.block_index);
            std::size_t byte_count = mapping.element_count * block_layout.stride();

            std::vector<unsigned char>& block_data = new_data_store.block_data(mapping.block_index);
            block_data.clear();
            block_data.insert(block_data.end(), data.begin() + data_offset, data.end());

            line_start_it += byte_count;
        }

        return new_data_store;
    }

    bool ply_parser::parse_data_format(const std::vector<std::string_view> parts, ply_data_format& data_format) const
    {
        data_format = ply_data_format::unknown;

        // A format statement must consist of the following parts: "format <ply_data_format> <version>"
        if (parts.size() < 3)
        {
            return false;
        }

        // Only binary ply files are supported
        if (parts[1] == "binary_little_endian")
        {
            data_format = ply_data_format::binary_little_endian;
        }
        else if (parts[1] == "binary_big_endian")
        {
            data_format = ply_data_format::binary_big_endian;
        }
        else
        {
            return false;
        }

        // Only ply version 1.0 files are supported
        if (parts[2] != "1.0")
        {
            return false;
        }

        return true;
    }

    bool ply_parser::parse_element(const std::vector<std::string_view> parts, block_mapping& mapping) const
    {
        // An element statement must consist of the following parts: "element <name> <element_count>"
        if (parts.size() < 3)
        {
            return false;
        }

        std::string_view section_name = parts[1];

        std::uint32_t count = 0;
        auto [ptr, error_code] = std::from_chars(parts[2].data(), parts[2].data() + parts[2].size(), count);
        if (error_code != std::errc())
        {
            return false;
        }

        mapping = block_mapping{ .name = section_name, .element_count = count };
        return true;
    }

    bool ply_parser::parse_property(
        const std::vector<std::string_view> parts, geometry::data_layout& current_block_layout) const
    {
        // A property statement can contain a list. This corresponds with: "property list ...".
        if (parts.size() < 2)
        {
            return false;
        }

        if (parts[1] == "list")
        {
            // A property list statement must consist of the following parts: "property list
            // <ply_data_format> <ply_data_format> <name>".
            if (parts.size() < 5)
            {
                return false;
            }

            geometry::data_element_format element_format = convert_from_string(parts[3]);
            if (element_format == geometry::data_element_format::unknown)
            {
                return false;
            }

            geometry::data_element_format datatype_list = convert_from_string(parts[2]);
            if (datatype_list == geometry::data_element_format::unknown)
            {
                return false;
            }

            std::string_view property_name = parts[4];

            // TODO: Finish implementation for list properties
            // current_block_layout.add_element_back(property_name, element_format, ..., ..., ...);
            throw std::runtime_error{ "Not implemented" };
        }
        else
        {
            // A property statement must consist of the following parts: "property <ply_data_format>
            // <name>".
            if (parts.size() < 3)
            {
                return false;
            }

            geometry::data_element_format element_format = convert_from_string(parts[1]);
            if (element_format == geometry::data_element_format::unknown)
            {
                return false;
            }

            std::string_view property_name = parts[2];
            std::size_t element_byte_count = format_byte_count(element_format);

            current_block_layout.add_element_back(
                property_name, element_format, element_byte_count, element_byte_count);

            return true;
        }
    }

    geometry::data_element_format ply_parser::convert_from_string(const std::string_view datatype) const
    {
        if (datatype == "char")
        {
            return geometry::data_element_format::signed_char;
        }
        else if (datatype == "uchar")
        {
            return geometry::data_element_format::unsigned_char;
        }
        else if (datatype == "short")
        {
            return geometry::data_element_format::signed_short;
        }
        else if (datatype == "ushort")
        {
            return geometry::data_element_format::unsigned_short;
        }
        else if (datatype == "int")
        {
            return geometry::data_element_format::signed_int;
        }
        else if (datatype == "uint")
        {
            return geometry::data_element_format::unsigned_int;
        }
        else if (datatype == "float")
        {
            return geometry::data_element_format::single_float;
        }
        else if (datatype == "double")
        {
            return geometry::data_element_format::double_float;
        }

        return geometry::data_element_format::unknown;
    }

    std::size_t ply_parser::format_byte_count(const geometry::data_element_format format) const
    {
        switch (format)
        {
        case geometry::data_element_format::signed_char:
        case geometry::data_element_format::unsigned_char:
            return 1;

        case geometry::data_element_format::signed_short:
        case geometry::data_element_format::unsigned_short:
            return 2;

        case geometry::data_element_format::signed_int:
        case geometry::data_element_format::unsigned_int:
            return 4;

        case geometry::data_element_format::single_float:
            return 4;

        case geometry::data_element_format::double_float:
            return 8;

        default:
            return 0;
        }
    }
} // namespace data_formats::ply
