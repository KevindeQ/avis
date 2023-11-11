#ifndef MIDDLEWARE_CONFIGURATION_CONFIGURATION_VISITOR_H
#define MIDDLEWARE_CONFIGURATION_CONFIGURATION_VISITOR_H

#include "avis/core/common.h"
#include "avis/middleware/configuration/configuration_ast.h"

namespace configuration
{
    template<typename derived_type>
    class basic_visitor
    {
    private:
        template<class... types>
        struct overload : types...
        {
            using types::operator()...;
        };
        template<class... types>
        overload(types...) -> overload<types...>;

        template<typename visitor>
        class generic_visitor : public visitor
        {
        private:
            template<typename argument_type, typename = void>
            struct has_visit_function : std::false_type
            {};
            template<typename argument_type>
            struct has_visit_function<argument_type, std::void_t<decltype(visit(std::declval<argument_type&>()))>> :
                std::true_type
            {};

            template<typename node_type>
            bool call_visit(node_type& node)
            {
                if constexpr (has_visit_function<node_type>::value)
                {
                    return visit(node);
                }
                else
                {
                    return true;
                }
            }

        public:
            using visitor::visit;

            bool operator()(recursive_wrapper<key_value_decl>& key_value_pair_node)
            {
                return call_visit<key_value_decl>(key_value_pair_node);
            }
            bool operator()(recursive_wrapper<object_decl>& object_node)
            {
                return call_visit<object_decl>(object_node);
            }
            bool operator()(recursive_wrapper<array_decl>& array_node)
            {
                return call_visit<array_decl>(array_node);
            }
            bool operator()(identifier_expr& identifier_node)
            {
                return call_visit(identifier_node);
            }
            bool operator()(string_expr& string_node)
            {
                return call_visit(string_node);
            }
            bool operator()(number_expr& number_node)
            {
                return call_visit(number_node);
            }
            bool operator()(bool_expr& bool_node)
            {
                return call_visit(bool_node);
            }
            bool operator()(null_expr& null_node)
            {
                return call_visit(null_node);
            }
        };

    public:
        void traverse_ast(value_type& ast_node);

    protected:
        bool traverse_key_value_decl(recursive_wrapper<key_value_decl>& key_value_pair_node);
        bool traverse_object_decl(recursive_wrapper<object_decl>& object_node);
        bool traverse_array_decl(recursive_wrapper<array_decl>& array_node);
        bool traverse_identifier_expr(identifier_expr& identifier_node);
        bool traverse_string_expr(string_expr& string_node);
        bool traverse_number_expr(number_expr& number_node);
        bool traverse_bool_expr(bool_expr& bool_node);
        bool traverse_null_expr(null_expr& null_node);

        template<typename node_type>
        bool traverse_node(node_type& node);

    private:
        bool dispatch_to_traverse(value_type& node);
        bool dispatch_to_visitor(value_type& node);
    };

    template<typename derived_type>
    inline void basic_visitor<derived_type>::traverse_ast(value_type& ast_node)
    {
        dispatch_to_traverse(ast_node);
    }

    template<typename derived_type>
    inline bool
        basic_visitor<derived_type>::traverse_key_value_decl(recursive_wrapper<key_value_decl>& key_value_pair_node)
    {
        value_type temp_node{key_value_pair_node};
        if (!dispatch_to_visitor(temp_node))
        {
            return false;
        }

        auto& temp_kv_decl = key_value_pair_node.get();
        if (!traverse_string_expr(temp_kv_decl.key_))
        {
            return false;
        }
        if (!dispatch_to_traverse(temp_kv_decl.value_))
        {
            return false;
        }

        return true;
    }

    template<typename derived_type>
    inline bool basic_visitor<derived_type>::traverse_object_decl(recursive_wrapper<object_decl>& object_node)
    {
        value_type temp_node{object_node};
        if (!dispatch_to_visitor(temp_node))
        {
            return false;
        }

        auto& temp_object_node = object_node.get();
        for (auto& key_value_pair : temp_object_node.key_value_pairs_)
        {
            recursive_wrapper temp_wrapper{key_value_pair};
            if (!traverse_key_value_decl(temp_wrapper))
            {
                return false;
            }
        }

        return true;
    }

    template<typename derived_type>
    inline bool basic_visitor<derived_type>::traverse_array_decl(recursive_wrapper<array_decl>& array_node)
    {
        value_type temp_node{array_node};
        if (!dispatch_to_visitor(temp_node))
        {
            return false;
        }

        auto& temp_array_node = array_node.get();
        for (auto& element : temp_array_node.values_)
        {
            if (!dispatch_to_traverse(element))
            {
                return false;
            }
        }

        return true;
    }

    template<typename derived_type>
    inline bool basic_visitor<derived_type>::traverse_identifier_expr(identifier_expr& identifier_node)
    {
        value_type temp_node{identifier_node};
        return dispatch_to_visitor(temp_node);
    }

    template<typename derived_type>
    inline bool basic_visitor<derived_type>::traverse_string_expr(string_expr& string_node)
    {
        value_type temp_node{string_node};
        return dispatch_to_visitor(temp_node);
    }

    template<typename derived_type>
    inline bool basic_visitor<derived_type>::traverse_number_expr(number_expr& number_node)
    {
        value_type temp_node{number_node};
        return dispatch_to_visitor(temp_node);
    }

    template<typename derived_type>
    inline bool basic_visitor<derived_type>::traverse_bool_expr(bool_expr& bool_node)
    {
        value_type temp_node{bool_node};
        return dispatch_to_visitor(temp_node);
    }

    template<typename derived_type>
    inline bool basic_visitor<derived_type>::traverse_null_expr(null_expr& null_node)
    {
        value_type temp_node{null_node};
        return dispatch_to_visitor(temp_node);
    }

    template<typename derived_type>
    template<typename node_type>
    inline bool basic_visitor<derived_type>::traverse_node(node_type& node)
    {
        value_type temp_node{node};
        return dispatch_to_visitor(temp_node);
    }

    template<typename derived_type>
    inline bool basic_visitor<derived_type>::dispatch_to_traverse(value_type& node)
    {
        return std::visit(
            overload{
                [this](recursive_wrapper<key_value_decl>& node) -> bool { return traverse_key_value_decl(node); },
                [this](recursive_wrapper<object_decl>& node) -> bool { return traverse_object_decl(node); },
                [this](recursive_wrapper<array_decl>& node) -> bool { return traverse_array_decl(node); },
                [this](identifier_expr& node) -> bool { return traverse_identifier_expr(node); },
                [this](string_expr& node) -> bool { return traverse_string_expr(node); },
                [this](number_expr& node) -> bool { return traverse_number_expr(node); },
                [this](bool_expr& node) -> bool { return traverse_bool_expr(node); },
                [this](null_expr& node) -> bool { return traverse_null_expr(node); },
            },
            node);
    }

    template<typename derived_type>
    inline bool basic_visitor<derived_type>::dispatch_to_visitor(value_type& node)
    {
        return std::visit(generic_visitor<derived_type>{}, node);
    }
} // namespace configuration

#endif
