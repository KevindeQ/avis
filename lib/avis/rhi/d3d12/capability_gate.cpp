#include "avis/rhi/d3d12/capability_gate.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/error.h"

#include <format>
#include <string>
#include <string_view>

namespace avis::rhi::d3d12
{
    namespace
    {
        avis::error
            rejected(const std::string_view check, const std::string_view required, const std::string_view reported)
        {
            return rhi::make_error(
                error_kind::unsupported_hardware,
                std::format("capability gate: {} requires {}, the adapter reports {}", check, required, reported));
        }

        std::string shader_model_name(const D3D_SHADER_MODEL model)
        {
            const int value = static_cast<int>(model);
            return std::format("{}.{}", value >> 4, value & 0xF);
        }
    } // namespace

    result<void> check_capabilities(device_interface& device, hardware_capabilities& capabilities)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL shader_model{ D3D_SHADER_MODEL_6_6 };
        if (FAILED(device.CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model))) ||
            shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        {
            return std::unexpected(rejected("shader model", "6.6", shader_model_name(shader_model.HighestShaderModel)));
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (auto queried = check(
                device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
                "CheckFeatureSupport(D3D12_OPTIONS)");
            !queried)
        {
            return queried;
        }
        if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
        {
            return std::unexpected(rejected(
                "resource binding tier",
                "tier 3",
                std::format("tier {}", static_cast<int>(options.ResourceBindingTier))));
        }
        if (options.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2)
        {
            return std::unexpected(rejected(
                "resource heap tier", "tier 2", std::format("tier {}", static_cast<int>(options.ResourceHeapTier))));
        }
        if (!options.TypedUAVLoadAdditionalFormats)
        {
            return std::unexpected(rejected("typed UAV loads", "support for additional formats", "no support"));
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
        if (FAILED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))) ||
            !options12.EnhancedBarriersSupported)
        {
            return std::unexpected(rejected("enhanced barriers", "support", "no support"));
        }

        // A closed list straight from the device, without an allocator, just to ask for the interface
        com_ptr<graphics_list_interface> list;
        if (FAILED(device.CreateCommandList1(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&list))))
        {
            return std::unexpected(rejected("interfaces", "ID3D12GraphicsCommandList7", "no support"));
        }

        // Optional capabilities
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
        if (SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))))
        {
            capabilities.minimum_wave_lane_count = options1.WaveLaneCountMin;
            capabilities.maximum_wave_lane_count = options1.WaveLaneCountMax;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
        if (SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3))))
        {
            capabilities.copy_queue_timestamps = options3.CopyQueueTimestampQueriesSupported != FALSE;
        }

        // The graphics queue's timestamp frequency, from a short-lived queue; device_queues reads its own (6.4)
        const D3D12_COMMAND_QUEUE_DESC queue_description{ .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
        com_ptr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(device.CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue))))
        {
            UINT64 frequency = 0;
            if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)))
            {
                capabilities.timestamp_frequency = frequency;
            }
        }
        return {};
    }
} // namespace avis::rhi::d3d12
