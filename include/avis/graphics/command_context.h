#ifndef GRAPHICS_COMMAND_CONTEXT_H
#define GRAPHICS_COMMAND_CONTEXT_H

#include "avis/common.h"

namespace graphics
{
    class command_context
    {
    public:
    protected:
        com_ptr<ID3D12GraphicsCommandList4> command_list;
    };

    class copy_context : public command_context
    {
    public:
        static copy_context begin(const std::string name = "");
    };

    class compute_context : public copy_context
    {
    public:
        static compute_context begin(const std::string name = "");
    };

    class graphics_context : public compute_context
    {
    public:
        static graphics_context begin(const std::string name = "");
    };
} // namespace graphics

#endif
