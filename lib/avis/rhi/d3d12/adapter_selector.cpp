#include "avis/rhi/d3d12/adapter_selector.h"

#include "avis/log.h"
#include "avis/rhi/d3d12/capability_gate.h"
#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/debug.h"
#include "avis/rhi/error.h"

#include <format>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        result<adapter_candidate> describe(com_ptr<adapter_interface> adapter)
        {
            DXGI_ADAPTER_DESC3 description{};
            if (auto described = check(adapter->GetDesc3(&description), "IDXGIAdapter4::GetDesc3"); !described)
            {
                return std::unexpected(std::move(described.error()));
            }

            adapter_candidate candidate;
            candidate.adapter = std::move(adapter);
            candidate.name = to_utf8(description.Description);
            candidate.luid =
                adapter_luid{ .low = description.AdapterLuid.LowPart, .high = description.AdapterLuid.HighPart };
            candidate.vendor_id = description.VendorId;
            candidate.device_id = description.DeviceId;
            candidate.dedicated_video_memory = description.DedicatedVideoMemory;
            candidate.software = (description.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0;
            return candidate;
        }

        std::string luid_text(const adapter_luid luid)
        {
            return std::format("{:08X}:{:08X}", static_cast<std::uint32_t>(luid.high), luid.low);
        }
    } // namespace

    adapter_selector::adapter_selector(factory_interface& factory, const adapter_configuration& configuration) :
        factory_{ factory },
        configuration_{ configuration }
    {}

    result<std::vector<adapter_candidate>> adapter_selector::enumerate() const
    {
        std::vector<adapter_candidate> candidates;

        if (configuration_.adapter.has_value())
        {
            const adapter_luid luid = *configuration_.adapter;
            com_ptr<adapter_interface> adapter;
            const HRESULT result = factory_.EnumAdapterByLuid(LUID{ luid.low, luid.high }, IID_PPV_ARGS(&adapter));
            if (FAILED(result))
            {
                return std::unexpected(
                    rhi::make_error(
                        error_kind::adapter_not_found,
                        std::format(
                            "no adapter with LUID {} ({})",
                            luid_text(luid),
                            d3d12::make_error(result, "EnumAdapterByLuid").message)));
            }
            auto candidate = describe(std::move(adapter));
            if (!candidate)
            {
                return std::unexpected(std::move(candidate.error()));
            }
            candidates.push_back(std::move(*candidate));
            return candidates;
        }

        for (UINT index = 0;; ++index)
        {
            com_ptr<adapter_interface> adapter;
            const HRESULT result = factory_.EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (auto enumerated = check(result, "EnumAdapterByGpuPreference"); !enumerated)
            {
                return std::unexpected(std::move(enumerated.error()));
            }
            auto candidate = describe(std::move(adapter));
            if (!candidate)
            {
                return std::unexpected(std::move(candidate.error()));
            }
            if (!candidate->software) // WARP only through EnumWarpAdapter, last
            {
                candidates.push_back(std::move(*candidate));
            }
        }

        if (configuration_.allow_software_adapter)
        {
            com_ptr<adapter_interface> warp;
            if (auto enumerated = check(factory_.EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter"); !enumerated)
            {
                return std::unexpected(std::move(enumerated.error()));
            }
            auto candidate = describe(std::move(warp));
            if (!candidate)
            {
                return std::unexpected(std::move(candidate.error()));
            }
            candidates.push_back(std::move(*candidate));
        }
        return candidates;
    }

    result<selected_adapter> adapter_selector::select()
    {
        rejections_.clear();

        auto candidates = enumerate();
        if (!candidates)
        {
            return std::unexpected(std::move(candidates.error()));
        }
        if (candidates->empty())
        {
            return std::unexpected(
                rhi::make_error(
                    error_kind::adapter_not_found,
                    configuration_.allow_software_adapter ?
                        "no adapter found" :
                        "no hardware adapter found (software adapters are not allowed)"));
        }

        for (auto& candidate : *candidates)
        {
            auto selected = try_candidate(candidate);
            if (selected)
            {
                log::get("rhi")->info(
                    "using adapter '{}' (LUID {}{})",
                    candidate.name,
                    luid_text(candidate.luid),
                    candidate.software ? ", software" : "");
                return selected;
            }
            log::get("rhi")->info("adapter '{}' rejected: {}", candidate.name, selected.error().message);
            rejections_.push_back({ std::move(candidate), std::move(selected.error()) });
        }
        return std::unexpected(failure());
    }

    std::span<const adapter_rejection> adapter_selector::rejections() const noexcept
    {
        return rejections_;
    }

    result<selected_adapter> adapter_selector::try_candidate(const adapter_candidate& candidate) const
    {
        com_ptr<ID3D12Device> base_device;
        if (auto created = check(
                D3D12CreateDevice(candidate.adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&base_device)),
                "D3D12CreateDevice(D3D_FEATURE_LEVEL_12_0)");
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }

        selected_adapter selected;
        if (FAILED(base_device.as(selected.device)))
        {
            return std::unexpected(
                rhi::make_error(
                    error_kind::unsupported_hardware,
                    "capability gate: interfaces requires ID3D12Device10, the device doesn't support it"));
        }

        selected.capabilities.adapter_name = candidate.name;
        selected.capabilities.luid = candidate.luid;
        selected.capabilities.vendor_id = candidate.vendor_id;
        selected.capabilities.device_id = candidate.device_id;
        selected.capabilities.dedicated_video_memory = candidate.dedicated_video_memory;
        selected.capabilities.software_adapter = candidate.software;

        if (auto passed = check_capabilities(*selected.device.get(), selected.capabilities); !passed)
        {
            return std::unexpected(std::move(passed.error()));
        }
        selected.adapter = candidate.adapter;
        return selected;
    }

    avis::error adapter_selector::failure() const
    {
        std::string message = "no adapter passed the RHI's capability gate:";
        for (const auto& rejection : rejections_)
        {
            message += std::format(
                "\n  - '{}' (LUID {}): {}",
                rejection.candidate.name,
                luid_text(rejection.candidate.luid),
                rejection.reason.message);
        }
        return rhi::make_error(error_kind::unsupported_hardware, std::move(message));
    }
} // namespace avis::rhi::d3d12
