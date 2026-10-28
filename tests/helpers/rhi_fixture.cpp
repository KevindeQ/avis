#include "rhi_fixture.h"

#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/adapter_selector.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/unique_handle.h"
#include "avis/rhi/error.h"

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <format>
#include <string_view>
#include <utility>

namespace avis::tests
{
    namespace
    {
        constexpr std::string_view graphics_tools_hint =
            "The D3D12 debug layer comes with the Windows \"Graphics Tools\" optional feature. Install it from an "
            "elevated PowerShell with:\n"
            "    Add-WindowsCapability -Online -Name Tools.Graphics.DirectX~~~~0.0.1.0";

        bool warp_requested()
        {
            char value[16]{};
            const DWORD length = GetEnvironmentVariableA("AVIS_TEST_ADAPTER", value, sizeof(value));
            return length > 0 && length < sizeof(value) && std::string_view{ value, length } == "warp";
        }
    } // namespace

    std::optional<rhi::adapter_luid> warp_luid()
    {
        rhi::com_ptr<rhi::d3d12::factory_interface> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        {
            return std::nullopt;
        }
        const rhi::d3d12::adapter_selector selector{ *factory.get(), { .allow_software_adapter = true } };
        const auto candidates = selector.enumerate();
        if (!candidates)
        {
            return std::nullopt;
        }
        for (const auto& candidate : *candidates)
        {
            if (candidate.software)
            {
                return candidate.luid;
            }
        }
        return std::nullopt;
    }

    rhi::render_hardware_interface_configuration
        test_rhi_configuration(rhi::render_hardware_interface_configuration overrides)
    {
        overrides.adapter.allow_software_adapter = true;
        overrides.debug.debug_layer = true;
        if (!overrides.adapter.adapter.has_value() && warp_requested())
        {
            overrides.adapter.adapter = warp_luid();
        }
        return overrides;
    }

    test_rhi::test_rhi(rhi::render_hardware_interface_configuration overrides)
    {
        auto created = rhi::render_hardware_interface::create(test_rhi_configuration(std::move(overrides)));
        if (!created)
        {
            if (rhi::is_error(created.error(), rhi::error_kind::debug_layer_unavailable))
            {
                FAIL(std::format("{}\n{}", created.error(), graphics_tools_hint));
            }
            FAIL(std::format("render_hardware_interface::create failed: {}", created.error()));
        }
        rhi_ = std::move(*created);
    }

    test_rhi::~test_rhi()
    {
        rhi_.reset();
        // Not while unwinding from a failed REQUIRE: that test has already failed
        if (!errors_accepted_ && std::uncaught_exceptions() == 0)
        {
            const auto errors = error_lines();
            CHECK(errors.empty());
        }
    }

    std::size_t test_rhi::wait_for(const rhi::synchronization_point point)
    {
        rhi::d3d12::device_queue& queue = rhi_->state().queues()[point.queue];
        if (!queue.is_complete(point.value))
        {
            const rhi::d3d12::unique_handle event{ CreateEventW(nullptr, FALSE, FALSE, nullptr) };
            REQUIRE(event);
            REQUIRE(SUCCEEDED(queue.fence().SetEventOnCompletion(point.value, event.get())));
            REQUIRE(WaitForSingleObject(event.get(), 10'000) == WAIT_OBJECT_0);
        }
        return rhi::completion_driver{ *rhi_ }.poll_completions();
    }

    std::vector<std::string> test_rhi::error_lines() const
    {
        std::vector<std::string> errors;
        for (auto& line : capture_.lines())
        {
            if (line.starts_with("[error]") || line.starts_with("[critical]"))
            {
                errors.push_back(std::move(line));
            }
        }
        return errors;
    }
} // namespace avis::tests
