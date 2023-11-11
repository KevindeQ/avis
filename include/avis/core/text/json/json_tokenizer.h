/*
 * Project: Application - D3D12 Tech Demo
 * Copyright (c) 2019 Kevin de Quillettes
 */

#pragma once

#include "avis/core/common.h"

namespace text::json
{
    enum class token_type
    {
        none,
        unknown,
        source_end,

        whitespace,

        identifier,
        numeric_literal,
        string_literal,

        // punctuation
        open_brace,
        close_brace,
        open_bracket,
        close_bracket,
        open_parenthesis,
        close_parenthesis,
        colon,
        comma,
        equals,

        keyword_true,
        keyword_false,
        keyword_null
    };

    struct token
    {
    public:
        token_type type;

        std::string_view text;
        std::size_t offset;
    };

    static const std::vector<std::tuple<const char*, token_type>> punctuation_table = {
      std::make_tuple("{", token_type::open_brace),
      std::make_tuple("}", token_type::close_brace),
      std::make_tuple("[", token_type::open_bracket),
      std::make_tuple("]", token_type::close_bracket),
      std::make_tuple("(", token_type::open_parenthesis),
      std::make_tuple(")", token_type::close_parenthesis),
      std::make_tuple(":", token_type::colon),       
      std::make_tuple(",", token_type::comma),
      std::make_tuple("=", token_type::equals),
    };

    static const std::vector<std::tuple<const char*, token_type>> keyword_table = {
      std::make_tuple("true", token_type::keyword_true),
      std::make_tuple("false", token_type::keyword_false),
      std::make_tuple("null", token_type::keyword_null)
    };

    class json_tokenizer
    {
    public:
        json_tokenizer(const std::string source, const bool skip_whitespace = true);

        std::optional<token> current_token() const { return current_token_; }
        std::optional<token> peek_token() const { return next_token_; }
        bool has_token() const { return current_token_.has_value(); }

        void advance();

    protected:
        virtual bool is_whitespace(char character) const;
        virtual bool is_synchronizing_char(char character) const;

    private:
        token parse_token();

        std::optional<token> parse_punctuation(std::string_view current_source);
        std::optional<token> parse_keyword(std::string_view current_source);
        std::optional<token> parse_identifier(std::string_view current_source);
        std::optional<token> parse_numeric_literal(std::string_view current_source);
        std::optional<token> parse_string_literal(std::string_view current_source);
        std::optional<token> parse_unknown_token(std::string_view current_source);

        inline void advance_character_stream(std::size_t count)
        {
            if (count > 0)
                untokenized_source_.remove_prefix(std::min(count, untokenized_source_.size()));
        }

        template<typename unary_predicate_t>
        inline void advance_character_stream(unary_predicate_t predicate)
        {
            std::size_t offset = 0;
            while (offset < untokenized_source_.length() && predicate(untokenized_source_[offset]))
                offset += 1;

            if (offset > 0)
                untokenized_source_.remove_prefix(std::min(offset, untokenized_source_.size()));
        }

        std::size_t get_current_offset() const { return untokenized_source_.data() - source_.data(); }

    private:
        std::string source_;
        std::string_view untokenized_source_;

        std::optional<token> current_token_;
        std::optional<token> next_token_;

        bool skip_whitespace_;
    };
}