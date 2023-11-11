#ifndef MIDDLEWARE_CONFIGURATION_INI_PARSER_H
#define MIDDLEWARE_CONFIGURATION_INI_PARSER_H

#include "avis/middleware/configuration/configuration_ast.h"
#include "avis/middleware/configuration/configuration_parser.h"

namespace configuration
{
    class ini_parser : public basic_parser
    {
    public:
        configuration parse(const std::string_view text) override;

    private:
        std::vector<std::string_view> parse_lines(const std::string_view& text);

        std::optional<object_decl> parse_top_level(const std::vector<std::string_view>& lines);
        bool parse_comment(const std::string_view& line);
        std::optional<identifier_expr> parse_section_header(const std::string_view& line);
        std::optional<key_value_decl> parse_key_value_pair(const std::string_view& line);
        std::optional<value_type> parse_value_expr(const std::string_view& value);

        char const* trim_left(char const* start);
        char const* trim_right(char const* end);

        void handle_parse_exception() noexcept;
    };
} // namespace configuration

#endif
