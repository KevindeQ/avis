#include "avis/runtime/graphics/command_context.h"

namespace graphics
{
    copy_context copy_context::begin(const std::string name)
    {
        return copy_context{};
    }

    compute_context compute_context::begin(const std::string name)
    {
        return compute_context{};
    }

    graphics_context graphics_context::begin(const std::string name)
    {
        return graphics_context{};
    }
} // namespace graphics
