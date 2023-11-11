#ifndef RUNTIME_GRAPHICS_GRAPHICS_SYSTEM_H
#define RUNTIME_GRAPHICS_GRAPHICS_SYSTEM_H

#include "avis/core/common.h"
#include "avis/runtime/graphics/graphics_device.h"

namespace graphics
{
    class graphics_system
    {
    public:
        graphics_system();
        ~graphics_system();

    private:
        static std::optional<graphics_device> device_;
        static std::once_flag device_init_flag_;
    };
} // namespace graphics

#endif
