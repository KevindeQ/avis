#include "ui/viewport_renderer.h"

#include <cstdint>
#include <format>
#include <rhi/qrhi.h>

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <Windows.h>
#include <wrl/client.h>

namespace avis::visualizer::ui
{
    std::optional<QString> enable_d3d12_debugging(const bool debug_layer, const bool gpu_based_validation)
    {
        if (!debug_layer && !gpu_based_validation)
        {
            return std::nullopt;
        }
        Microsoft::WRL::ComPtr<ID3D12Debug1> debug;
        if (const HRESULT result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug)); FAILED(result))
        {
            return QString::fromStdString(
                std::format("D3D12GetDebugInterface failed (HRESULT 0x{:08X})", static_cast<std::uint32_t>(result)));
        }
        // The same calls, in the same order, as the RHI's debug_state::enable_before_device
        debug->EnableDebugLayer();
        debug->SetEnableGPUBasedValidation(gpu_based_validation ? TRUE : FALSE);
        return std::nullopt;
    }

    std::optional<qt_adapter> read_qt_adapter(QRhi& rhi)
    {
        if (rhi.backend() != QRhi::D3D12)
        {
            return std::nullopt;
        }
        const auto* handles = static_cast<const QRhiD3D12NativeHandles*>(rhi.nativeHandles());
        if (handles == nullptr)
        {
            return std::nullopt;
        }
        return qt_adapter{
            .luid = { .low = static_cast<std::uint32_t>(handles->adapterLuidLow),
                      .high = static_cast<std::int32_t>(handles->adapterLuidHigh) },
            .name = QString::fromUtf8(rhi.driverInfo().deviceName),
        };
    }
} // namespace avis::visualizer::ui
