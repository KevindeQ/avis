#ifndef RUNTIME_GRAPHICS_GRAPHICS_DEVICE_H
#define RUNTIME_GRAPHICS_GRAPHICS_DEVICE_H

#include "avis/core/common.h"

namespace graphics
{
    class graphics_device
    {
    public:
        graphics_device(const bool use_warp, const bool enable_debug);
        ~graphics_device();

        inline const com_ptr<ID3D12Device>& native_ptr() const;

    private:
        void init_d3d12_device(const bool use_warp, const bool enable_debug);

        void enable_debug_layer();
        void disable_debug_layer();

        com_ptr<IDXGIAdapter1> get_hardware_adapter(
            com_ptr<IDXGIFactory4>& factory, bool request_high_perf_adapter);

        com_ptr<ID3D12Device> d3d12_device;
        com_ptr<ID3D12Debug> d3d12_debug_controller;
        com_ptr<ID3D12Debug4> d3d12_debug_controller4;
    };

    inline const com_ptr<ID3D12Device>& graphics_device::native_ptr() const
    {
        return d3d12_device;
    }
} // namespace graphics

#endif
