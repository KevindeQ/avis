#ifndef MIDDLEWARE_CONFIGURATION_CONFIGURATION_AST_H
#define MIDDLEWARE_CONFIGURATION_CONFIGURATION_AST_H

#include "avis/core/common.h"

namespace configuration
{
    template<typename type>
    struct recursive_wrapper
    {
    public:
        explicit recursive_wrapper(type element);
        recursive_wrapper(const recursive_wrapper& other);
        recursive_wrapper(recursive_wrapper&& other) noexcept;
        ~recursive_wrapper() = default;

        recursive_wrapper& operator=(recursive_wrapper other) noexcept;

        operator type&() noexcept;
        operator type const&() noexcept;

        type& get();
        type const& get() const;

        void swap(recursive_wrapper& other) noexcept;

    private:
        std::unique_ptr<type> placeholder_;
    };

    struct basic_expression
    {
    public:
        template<typename derived_type>
        friend class basic_visitor;
    };

    struct identifier_expr : public basic_expression
    {
    public:
        explicit identifier_expr(std::string&& identifier) : identifier_{std::forward<std::string>(identifier)} {}

        template<typename derived_type>
        friend class basic_visitor;

    private:
        std::string identifier_;
    };

    struct string_expr : public basic_expression
    {
    public:
        explicit string_expr(std::string&& value) : value_{std::forward<std::string>(value)} {}

        template<typename derived_type>
        friend class basic_visitor;

    private:
        std::string value_;
    };

    struct number_expr : public basic_expression
    {
    public:
        explicit number_expr(const float value) : value_{value} {}

        template<typename derived_type>
        friend class basic_visitor;

    private:
        float value_;
    };

    struct bool_expr : public basic_expression
    {
    public:
        explicit bool_expr(const bool value) : value_{value} {}

        template<typename derived_type>
        friend class basic_visitor;

    private:
        bool value_;
    };

    struct null_expr : public basic_expression
    {
    public:
        template<typename derived_type>
        friend class basic_visitor;
    };

    struct key_value_decl;
    struct object_decl;
    struct array_decl;

    using value_type = std::variant<
        identifier_expr,
        string_expr,
        number_expr,
        bool_expr,
        null_expr,
        recursive_wrapper<key_value_decl>,
        recursive_wrapper<object_decl>,
        recursive_wrapper<array_decl>>;

    struct basic_decl
    {
    public:
        template<typename derived_type>
        friend class basic_visitor;
    };

    struct key_value_decl : public basic_decl
    {
    public:
        key_value_decl(string_expr&& key, value_type&& value) :
            key_{std::forward<string_expr>(key)},
            value_{std::forward<value_type>(value)}
        {}

        template<typename derived_type>
        friend class basic_visitor;

    private:
        string_expr key_;
        value_type value_;
    };

    struct object_decl : public basic_decl
    {
    public:
        explicit object_decl(std::vector<key_value_decl>&& key_value_pairs) :
            key_value_pairs_{std::forward<std::vector<key_value_decl>>(key_value_pairs)}
        {}

        template<typename derived_type>
        friend class basic_visitor;

    private:
        std::vector<key_value_decl> key_value_pairs_;
    };

    struct array_decl : public basic_decl
    {
    public:
        explicit array_decl(std::vector<value_type>&& values) : values_{std::forward<std::vector<value_type>>(values)}
        {}

        template<typename derived_type>
        friend class basic_visitor;

    private:
        std::vector<value_type> values_;
    };

    template<typename type>
    inline recursive_wrapper<type>::recursive_wrapper(type element) :
        placeholder_{std::make_unique<type>(std::move(element))}
    {}

    template<typename type>
    inline recursive_wrapper<type>::recursive_wrapper(const recursive_wrapper& other) :
        placeholder_{std::make_unique<type>(*other.placeholder_)}
    {}

    template<typename type>
    inline recursive_wrapper<type>::recursive_wrapper(recursive_wrapper&& other) noexcept :
        placeholder_{std::move(other.placeholder_)}
    {}

    template<typename type>
    inline recursive_wrapper<type>& recursive_wrapper<type>::operator=(recursive_wrapper other) noexcept
    {
        *this = std::move(other);
        return *this;
    }

    template<typename type>
    inline recursive_wrapper<type>::operator type&() noexcept
    {
        return get();
    }

    template<typename type>
    inline recursive_wrapper<type>::operator type const&() noexcept
    {
        return get();
    }

    template<typename type>
    inline type& recursive_wrapper<type>::get()
    {
        return *placeholder_;
    }

    template<typename type>
    inline type const& recursive_wrapper<type>::get() const
    {
        return *placeholder_;
    }

    template<typename type>
    inline void recursive_wrapper<type>::swap(recursive_wrapper& other) noexcept
    {
        placeholder_.swap(other.placeholder_);
    }
} // namespace configuration

#endif
