#ifndef AVIS_RHI_D3D12_D3D12_H
#define AVIS_RHI_D3D12_D3D12_H

// The only include point for the Windows, DXGI and D3D12 headers inside the RHI (docs/design/09-rhi.md §20.2.1)

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
// The order matters: <windows.h> first (with the macros above), D3D12MemAlloc.h last
// clang-format off
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <D3D12MemAlloc.h>
// clang-format on

namespace avis::rhi::d3d12
{
    // The interface versions Avis uses; a version bump touches one line
    using device_interface = ID3D12Device10;
    using graphics_list_interface = ID3D12GraphicsCommandList7;
    using factory_interface = IDXGIFactory6;
    using adapter_interface = IDXGIAdapter4;
} // namespace avis::rhi::d3d12

#endif
