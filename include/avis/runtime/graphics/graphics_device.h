#ifndef RUNTIME_GRAPHICS_GRAPHICS_DEVICE_H
#define RUNTIME_GRAPHICS_GRAPHICS_DEVICE_H

#include "avis/core/common.h"

namespace graphics
{
    class graphics_device
    {
    public:
        graphics_device();
        ~graphics_device();

        inline ID3D12Device* native_ptr() const
        {
            return d3d12_device_;
        }

    private:
        ID3D12Device* d3d12_device_;
    };
} // namespace graphics

#endif
