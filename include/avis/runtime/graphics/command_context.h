#ifndef RUNTIME_GRAPHICS_COMMAND_CONTEXT_H
#define RUNTIME_GRAPHICS_COMMAND_CONTEXT_H

#include "avis/core/common.h"

namespace graphics
{
    class command_context
    {
    public:
    };

    class copy_context : public command_context
    {
    public:
        static ENGINE_API copy_context begin(const std::string name = "");
    };

    class compute_context : public copy_context
    {
    public:
        static ENGINE_API compute_context begin(const std::string name = "");
    };

    class graphics_context : public compute_context
    {
    public:
        static ENGINE_API graphics_context begin(const std::string name = "");
    };
} // namespace graphics

#endif
