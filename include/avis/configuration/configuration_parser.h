#ifndef CONFIGURATION_CONFIGURATION_PARSER_H
#define CONFIGURATION_CONFIGURATION_PARSER_H

#include "avis/common.h"
#include "avis/configuration/configuration_ast.h"

namespace configuration
{
    class configuration
    {
    public:
        explicit configuration(object_decl top_level_config);

        void bind();

        void merge(const configuration& other);

    private:
    };

    class basic_parser
    {
    public:
        virtual configuration parse(const std::string_view text) = 0;
    };
} // namespace configuration

#endif
