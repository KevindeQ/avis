#ifndef MIDDLEWARE_CONFIGURATION_CONFIGURATION_PARSER_H
#define MIDDLEWARE_CONFIGURATION_CONFIGURATION_PARSER_H

#include "avis/core/common.h"
#include "avis/middleware/configuration/configuration_ast.h"

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
