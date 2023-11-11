#include "avis/runtime/graphics/graphics_system.h"

namespace graphics
{
    std::optional<graphics_device> graphics_system::device_{std::nullopt};
    std::once_flag graphics_system::device_init_flag_{};

    graphics_system::graphics_system()
    {
        std::call_once(device_init_flag_, []() { device_ = graphics_device{}; });
    }

    graphics_system::~graphics_system() {}
} // namespace graphics
