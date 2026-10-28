#include "avis/rhi/d3d12/device.h"

#include "avis/log.h"
#include "avis/rhi/d3d12/adapter_selector.h"
#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/error.h"

#include <format>
#include <string>
#include <utility>

namespace avis::rhi::d3d12
{
    std::uint32_t windows_build() noexcept
    {
        using rtl_get_version = LONG(WINAPI*)(OSVERSIONINFOW*);

        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return 0;
        }
        const auto get_version =
            reinterpret_cast<rtl_get_version>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (get_version == nullptr)
        {
            return 0;
        }
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (get_version(&version) != 0)
        {
            return 0;
        }
        return version.dwBuildNumber;
    }

    result<std::unique_ptr<device>>
        device::create(const adapter_configuration& adapter, const debug_configuration& debug)
    {
        // Step 2: the OS
        const std::uint32_t build = windows_build();
        if (build < minimum_windows_build)
        {
            return std::unexpected(
                rhi::make_error(
                    error_kind::unsupported_os,
                    std::format(
                        "Windows build {} is too old: the RHI needs Windows 11 24H2 (build {}) or later",
                        build,
                        minimum_windows_build)));
        }

        std::unique_ptr<device> created{ new device };

        // Step 3: debug layer, GBV and DRED, before any device exists
        if (auto enabled = created->debug_.enable_before_device(debug); !enabled)
        {
            return std::unexpected(std::move(enabled.error()));
        }

        // Step 4: the factory
        const UINT factory_flags = created->debug_.debug_layer() ? DXGI_CREATE_FACTORY_DEBUG : 0;
        const HRESULT factory_result = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&created->factory_));
        if (factory_result == DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            return std::unexpected(
                rhi::make_error(
                    error_kind::debug_layer_unavailable,
                    "the DXGI debug layer is not installed; it comes with the Windows \"Graphics Tools\" optional "
                    "feature"));
        }
        if (auto factory_created = check(factory_result, "CreateDXGIFactory2"); !factory_created)
        {
            return std::unexpected(std::move(factory_created.error()));
        }

        // Steps 5–7: adapter, device, capability gate
        adapter_selector selector{ *created->factory_.get(), adapter };
        auto selected = selector.select();
        if (!selected)
        {
            return std::unexpected(std::move(selected.error()));
        }
        created->adapter_ = std::move(selected->adapter);
        created->device_ = std::move(selected->device);
        created->capabilities_ = std::move(selected->capabilities);

        // Step 8: info-queue routing
        if (auto attached = created->debug_.attach(*created->device_.get()); !attached)
        {
            return std::unexpected(std::move(attached.error()));
        }
        return created;
    }

    device::~device()
    {
        debug_.detach();
#ifndef NDEBUG
        if (device_ && debug_.debug_layer())
        {
            com_ptr<ID3D12DebugDevice> debug_device;
            if (SUCCEEDED(device_.as(debug_device)))
            {
                // Goes to the debugger output: the callback is detached. The device itself is still listed.
                debug_device->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
            }
        }
#endif
    }

    device_interface& device::native() noexcept
    {
        return *device_.get();
    }

    factory_interface& device::factory() noexcept
    {
        return *factory_.get();
    }

    adapter_interface& device::adapter() noexcept
    {
        return *adapter_.get();
    }

    const hardware_capabilities& device::capabilities() const noexcept
    {
        return capabilities_;
    }

    const debug_state& device::debug() const noexcept
    {
        return debug_;
    }

    bool device::lost() const noexcept
    {
        return lost_.load(std::memory_order_acquire);
    }

    void device::mark_removed(const std::string_view operation) noexcept
    {
        lost_.store(true, std::memory_order_release);
        try
        {
            std::call_once(
                removal_report_,
                [&]
                {
                    log::get("rhi")->error(
                        "the D3D12 device was removed during {}\n{}",
                        operation,
                        debug_.device_removed_report(*device_.get()));
                });
        }
        catch (...)
        {
            // The sticky flag still reports the removal
        }
    }

    void device::report_removed(const std::string_view operation)
    {
        mark_removed(operation);
        throw device_lost{ rhi::make_error(
            error_kind::device_lost, std::format("{}: the D3D12 device was removed", operation)) };
    }
} // namespace avis::rhi::d3d12
