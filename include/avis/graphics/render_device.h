#ifndef GRAPHICS_RENDER_DEVICE_H
#define GRAPHICS_RENDER_DEVICE_H

#include "avis/utils/com_ptr.h"

namespace graphics
{
    class render_device
    {
    public:
        render_device(const bool use_warp, const bool enable_debug);
        ~render_device();

        inline com_ptr<ID3D12Device> device_ptr() const
        {
            return d3d12_device;
        }

    private:
        void init_hardware_device_d3d12(const bool use_warp, const bool enable_debug);

        void enable_debug_layer();
        void disable_debug_layer();

        com_ptr<IDXGIAdapter1> get_hardware_adapter(com_ptr<IDXGIFactory4>& factory, bool request_high_perf_adapter);

        com_ptr<ID3D12Device> d3d12_device;
        com_ptr<ID3D12Debug> d3d12_debug_controller;
        com_ptr<ID3D12Debug4> d3d12_debug_controller4;
    };
} // namespace graphics

#endif
