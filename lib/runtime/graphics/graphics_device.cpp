#include "avis/runtime/graphics/graphics_device.h"

namespace graphics
{
    graphics_device::graphics_device(const bool use_warp, const bool enable_debug) :
        d3d12_device{ nullptr },
        d3d12_debug_controller{ nullptr }
    {
        if (enable_debug)
        {
            enable_debug_layer();
        }

        init_d3d12_device(use_warp, enable_debug);
    }

    graphics_device::~graphics_device()
    {
        if (d3d12_debug_controller)
        {
            disable_debug_layer();
        }
    }

    void graphics_device::init_d3d12_device(const bool use_warp, const bool enable_debug)
    {
        std::uint32_t dxgi_flags = 0;

        if (enable_debug)
        {
            dxgi_flags |= DXGI_CREATE_FACTORY_DEBUG;
        }

        com_ptr<IDXGIFactory4> factory{};
        com_ptr<IDXGIAdapter1> adapter{};
        throw_if_failed(CreateDXGIFactory2(dxgi_flags, IID_PPV_ARGS(&factory)));

        if (use_warp)
        {
            throw_if_failed(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        }
        else
        {
            adapter = get_hardware_adapter(factory, true);
            if (!adapter)
            {
                throw std::runtime_error{ "Could not find suitable adapter" };
            }
        }

        throw_if_failed(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12_device)));
    }

    void graphics_device::enable_debug_layer()
    {
        if (d3d12_debug_controller)
        {
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d3d12_debug_controller))))
            {
                if (!SUCCEEDED(d3d12_debug_controller.as(d3d12_debug_controller4)))
                {
                    d3d12_debug_controller4.reset();
                }
            }
            else
            {
                d3d12_debug_controller = nullptr;
            }
        }

        if (d3d12_debug_controller)
        {
            d3d12_debug_controller->EnableDebugLayer();
        }
    }

    void graphics_device::disable_debug_layer()
    {
        if (d3d12_debug_controller4)
        {
            d3d12_debug_controller4->DisableDebugLayer();
        }
    }

    com_ptr<IDXGIAdapter1> graphics_device::get_hardware_adapter(
        com_ptr<IDXGIFactory4>& factory, bool request_high_perf_adapter)
    {
        com_ptr<IDXGIAdapter1> adapter{};

        com_ptr<IDXGIFactory6> factory6;
        if (SUCCEEDED(factory.as(factory6)))
        {
            std::uint32_t adapter_index = 0;

            DXGI_GPU_PREFERENCE gpu_preference =
                request_high_perf_adapter ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_UNSPECIFIED;

            while (true)
            {
                if (!SUCCEEDED(factory6->EnumAdapterByGpuPreference(adapter_index, gpu_preference, IID_PPV_ARGS(&adapter))))
                {
                    break;
                }

                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);

                // Don't select the Basic Render Driver adapter
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                {
                    continue;
                }

                // Check to see whether the adapter supports Direct3D 12, but don't create the
                // actual device yet.
                if (SUCCEEDED(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr)))
                {
                    break;
                }

                ++adapter_index;
            }
        }

        if (!adapter)
        {

            std::uint32_t adapter_index = 0;
            while (true)
            {
                if (!SUCCEEDED(factory->EnumAdapters1(adapter_index, &adapter)))
                {
                    break;
                }

                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);

                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                {
                    // Don't select the Basic Render Driver adapter.
                    // If you want a software adapter, pass in "/warp" on the command line.
                    continue;
                }

                // Check to see whether the adapter supports Direct3D 12, but don't create the
                // actual device yet.
                if (SUCCEEDED(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr)))
                {
                    break;
                }

                ++adapter_index;
            }
        }

        return adapter;
    }
} // namespace graphics
