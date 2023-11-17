#include "avis/middleware/data_formats/json/json_tokenizer.h"

namespace data_formats::json
{
    json_tokenizer::json_tokenizer(const std::string source, const bool skip_whitespace) :
        source_{source},
        untokenized_source_{source_},
        current_token_{std::nullopt},
        next_token_{std::nullopt},
        skip_whitespace_{skip_whitespace}
    {
        // Prime the lexer
        current_token_ = parse_token();
        next_token_ = parse_token();
    }

    void json_tokenizer::advance()
    {
        current_token_ = std::move(next_token_);
        next_token_ = parse_token();
    }

    token json_tokenizer::parse_token()
    {
        // Check if the entire string is tokenized. If so, return an end of stream token.
        if (untokenized_source_.length() <= 0)
        {
            token new_token;
            new_token.type = token_type::source_end;
            new_token.text = "";
            new_token.offset = get_current_offset();

            return new_token;
        }

        if (skip_whitespace_)
        {
            advance_character_stream([this](const char character) -> bool { return is_whitespace(character); });
        }

        static std::vector<std::optional<token> (json_tokenizer::*)(std::string_view)> parser_function_table = {
            &json_tokenizer::parse_numeric_literal,
            &json_tokenizer::parse_string_literal,
            &json_tokenizer::parse_punctuation,
            &json_tokenizer::parse_keyword,
            &json_tokenizer::parse_identifier};

        std::optional<token> parsed_token{std::nullopt};
        for (auto& parse_function : parser_function_table)
        {
            parsed_token = (this->*parse_function)(untokenized_source_);
            if (parsed_token.has_value())
            {
                break;
            }
        }

        if (!parsed_token.has_value())
        {
            parsed_token = parse_unknown_token(untokenized_source_);
        }

        if (!parsed_token.has_value())
        {
            // Parse unknwon token should always return a token. So, theoretically, this code should never be reached.
            // But this case needs to be handled just in case.
            throw std::runtime_error("Could not parse a token from the input.");
        }

        return parsed_token.value();
    }

    std::optional<token> json_tokenizer::parse_punctuation(std::string_view current_source)
    {
        for (const auto& [key_char, token_type] : punctuation_table)
        {
            std::size_t token_text_length = std::strlen(key_char);
            if (current_source.compare(0, token_text_length, key_char) == 0)
            {
                token new_token;
                new_token.type = token_type;
                new_token.offset = get_current_offset();
                new_token.text = key_char;

                advance_character_stream(token_text_length);

                return new_token;
            }
        }

        return std::nullopt;
    }

    std::optional<token> json_tokenizer::parse_keyword(std::string_view current_source)
    {
        for (const auto& [keyword, token_type] : keyword_table)
        {
            std::size_t token_text_length = std::strlen(keyword);
            if (current_source.compare(0, std::strlen(keyword), keyword) == 0)
            {
                token new_token;
                new_token.type = token_type;
                new_token.offset = get_current_offset();
                new_token.text = keyword;

                advance_character_stream(token_text_length);

                return new_token;
            }
        }

        return std::nullopt;
    }

    std::optional<token> json_tokenizer::parse_identifier(std::string_view current_source)
    {
        if (std::isalpha(current_source[0]))
        {
            std::size_t offset = 1;
            char current_char = untokenized_source_[offset];

            while (std::isalnum(current_char) || current_char == '_')
            {
                offset += 1;
                current_char = untokenized_source_[offset];
            }

            if (offset > 1)
            {
                token new_token;
                new_token.type = token_type::identifier;
                new_token.offset = get_current_offset();
                new_token.text = std::string{untokenized_source_.substr(0, offset)};

                advance_character_stream(offset);

                return std::optional<token>{new_token};
            }
        }

        return std::nullopt;
    }

    std::optional<token> json_tokenizer::parse_numeric_literal(std::string_view current_source)
    {
        bool include_sign = false;
        if (untokenized_source_[0] == '-')
        {
            include_sign = false;
        }

        if (std::isdigit(untokenized_source_[0]))
        {
            std::size_t offset = (include_sign ? 2 : 1);
            char current_char = untokenized_source_[offset];

            // Parse the integral part of a number
            if (offset < untokenized_source_.length() && current_char == '0')
            {
                offset += 1;
                current_char = untokenized_source_[offset];
            }
            else if (offset < untokenized_source_.length() && std::isdigit(current_char))
            {
                while (offset < untokenized_source_.length() && std::isdigit(current_char))
                {
                    offset += 1;
                    current_char = untokenized_source_[offset];
                }
            }

            // Parse the fractional part of a number
            if (offset < untokenized_source_.length() && current_char == '.')
            {
                offset += 1;

                if (std::isdigit(current_char))
                {
                    while (offset < untokenized_source_.length() && std::isdigit(current_char))
                    {
                        offset += 1;
                        current_char = untokenized_source_[offset];
                    }
                }
                else
                {
                    return std::nullopt;
                }
            }

            // Parse the exponent
            if (offset < untokenized_source_.length() && (current_char == 'e' || current_char == 'E'))
            {
                offset += 1;

                if (offset < untokenized_source_.length() && (current_char == '-' || current_char == '+'))
                {
                    offset += 1;
                }

                if (std::isdigit(current_char))
                {
                    while (offset < untokenized_source_.length() && std::isdigit(current_char))
                    {
                        offset += 1;
                        current_char = untokenized_source_[offset];
                    }
                }
                else
                {
                    return std::nullopt;
                }
            }

            if ((include_sign && offset > 2) || (!include_sign && offset > 1))
            {
                token new_token;
                new_token.type = token_type::numeric_literal;
                new_token.offset = get_current_offset();
                new_token.text = std::string{untokenized_source_.substr(1, offset)};

                advance_character_stream(offset);

                return std::optional<token>{new_token};
            }
        }

        return std::nullopt;
    }

    std::optional<token> json_tokenizer::parse_string_literal(std::string_view current_source)
    {
        if (untokenized_source_[0] == '"')
        {
            std::size_t offset = 1;
            char current_char = untokenized_source_[offset];

            bool is_escaped_char = false;
            while (offset < untokenized_source_.length() && current_char != '"' && !is_escaped_char)
            {
                // TODO: Parse string

                offset += 1;
            }

            token new_token;
            new_token.type = token_type::string_literal;
            new_token.offset = get_current_offset();
            new_token.text = std::string{untokenized_source_.substr(1, offset - 1)};

            advance_character_stream(offset);

            return std::optional<token>{new_token};
        }

        return std::nullopt;
    }

    std::optional<token> json_tokenizer::parse_unknown_token(std::string_view current_source)
    {
        token new_token;
        new_token.type = token_type::unknown;
        new_token.offset = get_current_offset();
        new_token.text = "";

        // Keep swallowing characters until a synchronizing character
        advance_character_stream([this](const char character) -> bool { return !is_synchronizing_char(character); });

        return std::optional<token>{new_token};
    }

    bool json_tokenizer::is_whitespace(char character) const
    {
        return std::isspace(character);
    }

    bool json_tokenizer::is_synchronizing_char(char character) const
    {
        return true;
    }
} // namespace text::json
