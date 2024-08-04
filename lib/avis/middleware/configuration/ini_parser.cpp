#include "avis/middleware/configuration/ini_parser.h"

namespace configuration
{
    configuration ini_parser::parse(const std::string_view text)
    {
        std::vector<std::string_view> lines = parse_lines(text);
        std::optional<object_decl> parsed_object = parse_top_level(lines);
        if (!parsed_object.has_value())
        {
        }

        return configuration{parsed_object.value()};
    }

    std::vector<std::string_view> ini_parser::parse_lines(const std::string_view& text)
    {
        std::vector<std::string_view> lines;

        char const* line_start = text.data();
        char const* line_end = text.data();
        const char* text_end = text.data() + text.size();
        while (line_start < text_end)
        {
            while (line_end < text_end && *line_end != '\n')
            {
                line_end += 1;
            }

            char const* trimmed_line_start = trim_left(line_start);
            char const* trimmed_line_end = trim_right(line_end);
            lines.emplace_back(trimmed_line_start, std::distance(trimmed_line_start, trimmed_line_end));
            line_start = line_end;
        }

        return lines;
    }

    std::optional<object_decl> ini_parser::parse_top_level(const std::vector<std::string_view>& lines)
    {
        // object_decl top_level_structure{ };
        // std::optional<identifier_expr> section_name{ };
        // std::optional<object_decl> section{ };

        // for (auto& line : lines)
        //{
        //     try
        //     {
        //         if (parse_comment(line))
        //         {
        //             continue;
        //         }
        //         else if (auto parsed_identifier = parse_section_header(line); parsed_identifier.has_value())
        //         {
        //             if (section_name.has_value())
        //             {
        //                 //top_level_structure.add_key_value(section_name.value(), section.value());
        //             }

        //            section_name.emplace(parsed_identifier.value());
        //            section.emplace();
        //        }
        //        else if (auto parsed_kv_pair = parse_key_value_pair(line); parsed_kv_pair.has_value())
        //        {
        //            if (!section_name.has_value())
        //            {
        //                throw std::runtime_error{ "Parsed key-value pair not as part of any section." };
        //            }
        //            if (!section.has_value())
        //            {
        //                throw std::runtime_error{ "Section in-memory structure is not initialized." };
        //            }

        //            //section->add_key_value(parsed_kv_pair.value());
        //        }
        //        else
        //        {

        //        }
        //    }
        //    catch (...)
        //    {
        //        handle_parse_exception();
        //    }
        //}

        // return std::optional<object_decl>{ top_level_structure };
        return std::nullopt;
    }

    bool ini_parser::parse_comment(const std::string_view& line)
    {
        std::array<std::string_view, 2> prefixes{std::string_view{";"}, std::string_view{"#"}};

        for (auto& prefix : prefixes)
        {
            if (line.size() > 0 && line.compare(0, prefix.size(), prefix) == 0)
            {
                return true;
            }
        }

        return false;
    }

    std::optional<identifier_expr> ini_parser::parse_section_header(const std::string_view& line)
    {
        // A section header line must contain at least a open and close bracket, and a single character
        if (line.size() <= 2)
        {
            return std::nullopt;
        }

        std::string_view prefix{"["};
        if (line.compare(0, prefix.size(), prefix) == 0)
        {
            return std::nullopt;
        }
        std::string_view suffix{"]"};
        if (line.compare(line.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            return std::nullopt;
        }

        std::string_view identifier = line.substr(1, line.size() - 2);
        auto trimmed_identifier_start = trim_left(identifier.data());
        auto trimmed_identifier_end = trim_right(identifier.data() + identifier.size() - 1);

        std::size_t identifier_length = std::distance(trimmed_identifier_start, trimmed_identifier_end);
        if (identifier_length <= 0)
        {
            return std::nullopt;
        }

        return std::optional<identifier_expr>{std::string{trimmed_identifier_start, identifier_length}};
    }

    std::optional<key_value_decl> ini_parser::parse_key_value_pair(const std::string_view& line)
    {
        // A key-value line should at least contain an equals sign and 2 characters. One for the key and one for the
        // value
        if (line.size() <= 2)
        {
            return std::nullopt;
        }

        std::string divider{"="};
        auto divider_offset = line.find_first_of(divider);
        if (divider_offset == std::string_view::npos)
        {
            return std::nullopt;
        }

        std::string_view key = line.substr(0, divider_offset);
        std::string_view value = line.substr(divider_offset + 1, line.size() - divider_offset - 1);
        if (key.size() <= 0 || value.size() <= 0)
        {
            return std::nullopt;
        }

        auto value_expr = parse_value_expr(value);
        if (!value_expr.has_value())
        {
            return std::nullopt;
        }

        return std::optional<key_value_decl>{
            key_value_decl{std::move(string_expr{std::string{key}}), std::move(value_expr.value())}};
    }

    std::optional<value_type> ini_parser::parse_value_expr(const std::string_view& value)
    {
        if (value.compare("null") == 0)
        {
            return std::optional<value_type>{null_expr{}};
        }
        else if (value.compare("true") == 0)
        {
            return std::optional<value_type>{bool_expr{true}};
        }
        else if (value.compare("false") == 0)
        {
            return std::optional<value_type>{bool_expr{false}};
        }
        return std::optional<value_type>();
    }

    char const* ini_parser::trim_left(char const* start)
    {
        char const* current = start;
        while (std::isspace(*current))
        {
            ++current;
        }

        return current;
    }

    char const* ini_parser::trim_right(char const* end)
    {
        char const* current = end;
        while (std::isspace(*current))
        {
            --current;
        }

        return current;
    }

    void ini_parser::handle_parse_exception() noexcept
    {
        try
        {
            throw;
        }
        catch (const std::runtime_error& exception)
        {}
        catch (...)
        {}
    }
} // namespace configuration
