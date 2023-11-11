#include "avis/runtime/graphics/graphics_device.h"

namespace graphics
{
    graphics_device::graphics_device() : d3d12_device_{nullptr} {}

    graphics_device::~graphics_device()
    {
        safe_release(d3d12_device_);
    }
} // namespace graphics
