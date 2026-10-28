// Device creation, the capability gate, adapter selection and debug-layer routing (docs/tasks/06-rhi.md §6.3).
// Needs a D3D12 device (WARP is enough).

#include "helpers/log_capture.h"
#include "helpers/rhi_fixture.h"

#include "avis/rhi/d3d12/adapter_selector.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/error.h"
#include "avis/rhi/render_hardware_interface.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <string>

namespace
{
    using namespace avis::rhi;

    avis::rhi::com_ptr<d3d12::factory_interface> make_factory()
    {
        avis::rhi::com_ptr<d3d12::factory_interface> factory;
        REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
        return factory;
    }
} // namespace

TEST_CASE("create succeeds and reports the adapter's capabilities", "[rhi]")
{
    avis::tests::test_rhi hardware_interface;
    const hardware_capabilities& capabilities = hardware_interface->capabilities();

    CHECK_FALSE(capabilities.adapter_name.empty());
    CHECK((capabilities.luid.low != 0 || capabilities.luid.high != 0));
    CHECK(capabilities.timestamp_frequency > 0);
    CHECK(capabilities.minimum_wave_lane_count > 0);
    CHECK(capabilities.maximum_wave_lane_count >= capabilities.minimum_wave_lane_count);
    CHECK(hardware_interface.log().contains(std::format("using adapter '{}'", capabilities.adapter_name)));
}

TEST_CASE("WARP passes the capability gate", "[rhi]")
{
    // A regression check for every machine and runner: a WARP version that fails the gate fails here, with the
    // unsupported_hardware message naming the check
    const auto warp = avis::tests::warp_luid();
    REQUIRE(warp.has_value());

    render_hardware_interface_configuration configuration;
    configuration.adapter.adapter = warp;
    avis::tests::test_rhi hardware_interface{ configuration };

    CHECK(hardware_interface->capabilities().software_adapter);
    CHECK(hardware_interface->capabilities().luid == *warp);
}

TEST_CASE("adapter_selector::enumerate follows the selection order", "[rhi]")
{
    const auto factory = make_factory();

    SECTION("hardware only unless software adapters are allowed")
    {
        const d3d12::adapter_selector selector{ *factory.get(), {} };
        const auto candidates = selector.enumerate();
        REQUIRE(candidates.has_value());
        CHECK(std::ranges::none_of(*candidates, [](const auto& candidate) { return candidate.software; }));
    }

    SECTION("WARP comes last, once, when allowed")
    {
        const d3d12::adapter_selector selector{ *factory.get(), { .allow_software_adapter = true } };
        const auto candidates = selector.enumerate();
        REQUIRE(candidates.has_value());
        REQUIRE_FALSE(candidates->empty());
        CHECK(candidates->back().software);
        CHECK(std::ranges::count_if(*candidates, [](const auto& candidate) { return candidate.software; }) == 1);
    }

    SECTION("a configured LUID gives exactly that adapter")
    {
        const d3d12::adapter_selector all{ *factory.get(), { .allow_software_adapter = true } };
        const auto candidates = all.enumerate();
        REQUIRE(candidates.has_value());
        REQUIRE_FALSE(candidates->empty());

        for (const auto& expected : *candidates)
        {
            const d3d12::adapter_selector one{ *factory.get(), { .adapter = expected.luid } };
            const auto found = one.enumerate();
            REQUIRE(found.has_value());
            REQUIRE(found->size() == 1);
            CHECK(found->front().luid == expected.luid);
            CHECK(found->front().name == expected.name);
        }
    }

    SECTION("an unknown LUID fails with adapter_not_found")
    {
        const d3d12::adapter_selector selector{ *factory.get(),
                                                { .adapter = adapter_luid{ .low = 0xDEADBEEF, .high = 0x7FFF } } };
        const auto candidates = selector.enumerate();
        REQUIRE_FALSE(candidates.has_value());
        CHECK(is_error(candidates.error(), error_kind::adapter_not_found));
    }
}

TEST_CASE("invalid configurations fail before any device is created", "[rhi]")
{
    const avis::tests::log_capture capture{ "rhi" };

    const auto rejected = [&](render_hardware_interface_configuration configuration)
    {
        const auto created = render_hardware_interface::create(avis::tests::test_rhi_configuration(configuration));
        REQUIRE_FALSE(created.has_value());
        CHECK(is_error(created.error(), error_kind::invalid_argument));
        return created.error().message;
    };

    render_hardware_interface_configuration too_many_descriptors;
    too_many_descriptors.descriptor_heaps.persistent_descriptor_count = 900'000;
    too_many_descriptors.descriptor_heaps.transient_descriptor_count = 200'000;
    CHECK(rejected(too_many_descriptors).contains("1100000"));

    render_hardware_interface_configuration no_samplers;
    no_samplers.descriptor_heaps.sampler_count = 0;
    CHECK(rejected(no_samplers).contains("sampler_count"));

    render_hardware_interface_configuration too_many_samplers;
    too_many_samplers.descriptor_heaps.sampler_count = 4'096;
    CHECK(rejected(too_many_samplers).contains("sampler_count"));

    render_hardware_interface_configuration no_persistent;
    no_persistent.descriptor_heaps.persistent_descriptor_count = 0;
    CHECK(rejected(no_persistent).contains("persistent_descriptor_count"));

    // No adapter was tried: selection logs every adapter it uses or rejects
    CHECK_FALSE(capture.contains("adapter"));
}

TEST_CASE("a D3D12 validation error reaches the rhi logger at error level", "[rhi]")
{
    avis::tests::test_rhi hardware_interface;

    // A buffer with a height of 2 is invalid
    const D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = 256;
    description.Height = 2;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    avis::rhi::com_ptr<ID3D12Resource> resource;
    const HRESULT result = hardware_interface->state().device().native().CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
    CHECK(FAILED(result));

    const auto errors = hardware_interface.error_lines();
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front().starts_with("[error] D3D12: "));
    hardware_interface.accept_errors();
}
