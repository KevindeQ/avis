#include "avis/rhi/d3d12/debug.h"

#include "avis/log.h"
#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace avis::rhi::d3d12
{
    namespace
    {
        // Known-noisy messages that say nothing about Avis' correctness
        constexpr std::array<D3D12_MESSAGE_ID, 2> denied_messages{
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
        };

        spdlog::level::level_enum log_level(const D3D12_MESSAGE_SEVERITY severity) noexcept
        {
            switch (severity)
            {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:
                return spdlog::level::err;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                return spdlog::level::warn;
            case D3D12_MESSAGE_SEVERITY_INFO:
                return spdlog::level::debug;
            case D3D12_MESSAGE_SEVERITY_MESSAGE:
                return spdlog::level::trace;
            }
            return spdlog::level::err;
        }

        // Runs on whichever thread issued the D3D12 call. Only logs, and breaks into an attached debugger on errors
        // when configured.
        void CALLBACK on_message(
            D3D12_MESSAGE_CATEGORY,
            const D3D12_MESSAGE_SEVERITY severity,
            const D3D12_MESSAGE_ID id,
            const LPCSTR description,
            void* const context)
        {
            try
            {
                log::get("rhi")->log(
                    log_level(severity),
                    "D3D12: {} [message {}]",
                    description != nullptr ? description : "",
                    static_cast<int>(id));
            }
            catch (...)
            {
                // Never throw into D3D12
            }

            const auto* const state = static_cast<const debug_state*>(context);
            if (state != nullptr && state->break_on_error() && severity <= D3D12_MESSAGE_SEVERITY_ERROR &&
                IsDebuggerPresent())
            {
                DebugBreak();
            }
        }

        std::string name_or_unnamed(const wchar_t* const name)
        {
            return name != nullptr ? to_utf8(name) : std::string{ "<unnamed>" };
        }

        void append_breadcrumbs(std::string& report, const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1& output)
        {
            bool any = false;
            for (const D3D12_AUTO_BREADCRUMB_NODE1* node = output.pHeadAutoBreadcrumbNode; node != nullptr;
                 node = node->pNext)
            {
                const std::uint32_t count = node->BreadcrumbCount;
                const std::uint32_t completed = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
                if (completed >= count)
                {
                    continue; // the list finished; not where the device was lost
                }
                any = true;
                report += std::format(
                    "  command list '{}' on queue '{}': {} of {} operations completed\n",
                    name_or_unnamed(node->pCommandListDebugNameW),
                    name_or_unnamed(node->pCommandQueueDebugNameW),
                    completed,
                    count);

                const std::uint32_t first = completed >= 2 ? completed - 2 : 0;
                const std::uint32_t last = std::min(count, completed + 3);
                for (std::uint32_t index = first; index < last; ++index)
                {
                    report += std::format(
                        "    {} operation {}: D3D12_AUTO_BREADCRUMB_OP {}\n",
                        index == completed ? "->" : "  ",
                        index,
                        static_cast<int>(node->pCommandHistory[index]));
                }
            }
            if (!any)
            {
                report += "  no command list was left unfinished\n";
            }
        }

        void append_allocations(std::string& report, const char* const heading, const D3D12_DRED_ALLOCATION_NODE1* node)
        {
            report += std::format("  {}:\n", heading);
            if (node == nullptr)
            {
                report += "    none\n";
            }
            for (; node != nullptr; node = node->pNext)
            {
                report += std::format(
                    "    '{}' (D3D12_DRED_ALLOCATION_TYPE {})\n",
                    name_or_unnamed(node->ObjectNameW),
                    static_cast<int>(node->AllocationType));
            }
        }
    } // namespace

    debug_state::~debug_state()
    {
        detach();
    }

    result<void> debug_state::enable_before_device(const debug_configuration& configuration)
    {
        configuration_ = configuration;
        configuration_.debug_layer = configuration.debug_layer || configuration.gpu_based_validation;

        if (configuration_.debug_layer)
        {
            const HRESULT result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug_));
            if (FAILED(result))
            {
                return std::unexpected(
                    rhi::make_error(
                        error_kind::debug_layer_unavailable,
                        std::format(
                            "the D3D12 debug layer is not installed; it comes with the Windows \"Graphics Tools\" "
                            "optional feature ({})",
                            d3d12::make_error(result, "D3D12GetDebugInterface").message)));
            }
            debug_->EnableDebugLayer();
            // Set either way: the setting is process-wide and applies to devices created afterwards
            debug_->SetEnableGPUBasedValidation(configuration_.gpu_based_validation ? TRUE : FALSE);
        }

        if (configuration_.device_removed_extended_data)
        {
            com_ptr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
            {
                settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            }
            else
            {
                log::get("rhi")->warn("DRED settings are unavailable; a device removal will have no breadcrumbs");
            }
        }
        return {};
    }

    result<void> debug_state::attach(device_interface& device)
    {
        if (!configuration_.debug_layer)
        {
            return {};
        }

        if (FAILED(device.QueryInterface(IID_PPV_ARGS(&info_queue_))))
        {
            log::get("rhi")->warn("ID3D12InfoQueue1 is unavailable; D3D12 messages won't reach the \"rhi\" logger");
            return {};
        }

        std::array<D3D12_MESSAGE_ID, denied_messages.size()> denied = denied_messages;
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = static_cast<UINT>(denied.size());
        filter.DenyList.pIDList = denied.data();
        if (auto pushed = check(info_queue_->PushStorageFilter(&filter), "ID3D12InfoQueue::PushStorageFilter"); !pushed)
        {
            return pushed;
        }

        return check(
            info_queue_->RegisterMessageCallback(
                &on_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &callback_cookie_),
            "ID3D12InfoQueue1::RegisterMessageCallback");
    }

    void debug_state::detach() noexcept
    {
        if (info_queue_ && callback_cookie_ != 0)
        {
            info_queue_->UnregisterMessageCallback(callback_cookie_);
            callback_cookie_ = 0;
        }
        info_queue_.release();
    }

    std::string debug_state::device_removed_report(device_interface& device) const
    {
        std::string report = std::format(
            "device removed reason: {}\n", d3d12::make_error(device.GetDeviceRemovedReason(), "removal").message);

        com_ptr<ID3D12DeviceRemovedExtendedData1> data;
        if (FAILED(device.QueryInterface(IID_PPV_ARGS(&data))))
        {
            report += "DRED is unavailable\n";
            return report;
        }

        report += "auto-breadcrumbs:\n";
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        if (SUCCEEDED(data->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        {
            append_breadcrumbs(report, breadcrumbs);
        }
        else
        {
            report += "  unavailable\n";
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT1 page_fault{};
        if (SUCCEEDED(data->GetPageFaultAllocationOutput1(&page_fault)))
        {
            report += std::format("page fault at GPU address 0x{:X}\n", page_fault.PageFaultVA);
            append_allocations(report, "allocations alive at that address", page_fault.pHeadExistingAllocationNode);
            append_allocations(report, "allocations recently freed there", page_fault.pHeadRecentFreedAllocationNode);
        }
        else
        {
            report += "page fault data: unavailable\n";
        }
        return report;
    }

    bool debug_state::debug_layer() const noexcept
    {
        return configuration_.debug_layer;
    }

    bool debug_state::object_names() const noexcept
    {
        return configuration_.object_names;
    }

    bool debug_state::break_on_error() const noexcept
    {
        return configuration_.break_on_error;
    }

    void set_debug_name(const debug_state& debug, ID3D12Object& object, const std::string_view name) noexcept
    {
        if (!debug.object_names() || name.empty())
        {
            return;
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
        if (length <= 0)
        {
            return;
        }
        try
        {
            std::wstring wide(static_cast<std::size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), length);
            object.SetName(wide.c_str());
        }
        catch (...)
        {
            // A missing debug name is not worth failing for
        }
    }

    std::string to_utf8(const std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int length =
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
        return result;
    }
} // namespace avis::rhi::d3d12
