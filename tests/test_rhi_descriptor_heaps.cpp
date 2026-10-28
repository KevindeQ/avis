// Descriptor heaps, leases, descriptor ranges and the RTV/DSV view cache (docs/tasks/06-rhi.md §6.6). Needs a D3D12
// device (WARP is enough).

#include "helpers/rhi_fixture.h"

#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/descriptor_heaps.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/native_access.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/error.h"
#include "avis/rhi/resources.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using namespace avis;
using rhi::d3d12::descriptor_heaps;
using rhi::d3d12::descriptor_lease;

namespace
{
    rhi::render_hardware_interface_configuration small_heaps()
    {
        rhi::render_hardware_interface_configuration configuration;
        configuration.descriptor_heaps = { .persistent_descriptor_count = 64,
                                           .transient_descriptor_count = 32,
                                           .sampler_count = 16,
                                           .render_target_view_count = 8,
                                           .depth_stencil_view_count = 8 };
        return configuration;
    }

    // A raw buffer view over a buffer created without views, so the test controls every descriptor
    struct raw_buffer
    {
        explicit raw_buffer(tests::test_rhi& hardware_interface) :
            handle{ *rhi::resources{ *hardware_interface }.create_buffer(
                { .size = 256, .usage = rhi::buffer_usage::copy_destination, .debug_name = "raw buffer" }) },
            resource{ rhi::d3d12::native_resource(*hardware_interface, handle) }
        {}

        D3D12_SHADER_RESOURCE_VIEW_DESC srv() const noexcept
        {
            return { .Format = DXGI_FORMAT_R32_TYPELESS,
                     .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                     .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                     .Buffer = { .FirstElement = 0,
                                 .NumElements = 64,
                                 .StructureByteStride = 0,
                                 .Flags = D3D12_BUFFER_SRV_FLAG_RAW } };
        }

        rhi::buffer_handle handle;
        ID3D12Resource* resource;
    };
} // namespace

TEST_CASE("Descriptor heaps are sized from the configuration", "[rhi]")
{
    tests::test_rhi hardware_interface{ small_heaps() };
    descriptor_heaps& heaps = hardware_interface->state().descriptors();

    const auto visible = heaps.shader_visible_heaps();
    CHECK(visible[0]->GetDesc().NumDescriptors == 64 + 32); // persistent + transient
    CHECK(visible[1]->GetDesc().NumDescriptors == 16);
    CHECK(heaps.free_persistent_count() == 63); // index 0 is the null descriptor
    CHECK(heaps.free_sampler_count() == 15);

    const raw_buffer buffer{ hardware_interface };
    REQUIRE(buffer.resource != nullptr);
    auto lease = heaps.create_srv(*buffer.resource, buffer.srv());
    REQUIRE(lease.has_value());
    CHECK(lease->index().value == 1); // the persistent region starts at 1
}

TEST_CASE("Descriptor leases return their index when destroyed", "[rhi]")
{
    tests::test_rhi hardware_interface{ small_heaps() };
    descriptor_heaps& heaps = hardware_interface->state().descriptors();
    const raw_buffer buffer{ hardware_interface };
    const std::uint32_t free = heaps.free_persistent_count();

    {
        auto lease = heaps.create_srv(*buffer.resource, buffer.srv());
        REQUIRE(lease.has_value());
        CHECK(heaps.free_persistent_count() == free - 1);

        descriptor_lease moved{ std::move(*lease) };
        CHECK(lease->index().value == 0);
        lease = descriptor_lease{}; // destroying the moved-from lease returns nothing
        CHECK(heaps.free_persistent_count() == free - 1);
        CHECK(moved.index().value != 0);
    }
    CHECK(heaps.free_persistent_count() == free);

    SECTION("the persistent region fills up")
    {
        std::vector<descriptor_lease> leases;
        for (std::uint32_t index = 0; index < free; ++index)
        {
            auto lease = heaps.create_srv(*buffer.resource, buffer.srv());
            REQUIRE(lease.has_value());
            leases.push_back(std::move(*lease));
        }
        const auto full = heaps.create_srv(*buffer.resource, buffer.srv());
        REQUIRE_FALSE(full.has_value());
        CHECK(rhi::is_error(full.error(), rhi::error_kind::descriptor_heap_full));
    }
}

TEST_CASE("Descriptor ranges are carved from the transient region", "[rhi]")
{
    tests::test_rhi hardware_interface{ small_heaps() };
    descriptor_heaps& heaps = hardware_interface->state().descriptors();
    const raw_buffer buffer{ hardware_interface };

    auto first = heaps.create_range(16);
    auto second = heaps.create_range(16);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK((*first)->first == 64); // right after the persistent region
    CHECK((*second)->first == 80);

    const auto full = heaps.create_range(2);
    REQUIRE_FALSE(full.has_value());
    CHECK(rhi::is_error(full.error(), rhi::error_kind::descriptor_heap_full));

    // The first index is the range's null descriptor; views start after it
    auto view = heaps.create_transient_srv(**second, *buffer.resource, buffer.srv());
    REQUIRE(view.has_value());
    CHECK(view->value == 81);
    for (int index = 2; index < 16; ++index)
    {
        REQUIRE(heaps.create_transient_srv(**second, *buffer.resource, buffer.srv()).has_value());
    }
    const auto exhausted = heaps.create_transient_srv(**second, *buffer.resource, buffer.srv());
    REQUIRE_FALSE(exhausted.has_value());
    CHECK(rhi::is_error(exhausted.error(), rhi::error_kind::transient_memory_exhausted));

    // Destroying a range makes its indices reusable, merged with free neighbours
    first->reset();
    auto reused = heaps.create_range(16);
    REQUIRE(reused.has_value());
    CHECK((*reused)->first == 64);
    reused->reset();
    second->reset();
    auto whole = heaps.create_range(32);
    REQUIRE(whole.has_value());
    CHECK((*whole)->first == 64);

    const auto too_small = heaps.create_range(1);
    REQUIRE_FALSE(too_small.has_value());
    CHECK(rhi::is_error(too_small.error(), rhi::error_kind::invalid_argument));
}

TEST_CASE("The view cache creates each render target and depth view once", "[rhi]")
{
    tests::test_rhi hardware_interface{ small_heaps() };
    rhi::resources resources{ *hardware_interface };
    rhi::d3d12::resource_manager& manager = hardware_interface->state().resources();

    const auto target = resources.create_texture(
        { .width = 64,
          .height = 64,
          .mip_levels = 2,
          .format = rhi::format::rgba8_unorm,
          .usage = rhi::texture_usage::render_target,
          .debug_name = "target" });
    REQUIRE(target.has_value());
    const rhi::d3d12::texture_record* record = manager.find(*target);
    REQUIRE(record != nullptr);
    REQUIRE(record->views != nullptr);

    const auto first =
        record->views->render_target(*record->resource.get(), record->description, { .texture = *target });
    const auto again =
        record->views->render_target(*record->resource.get(), record->description, { .texture = *target });
    REQUIRE(first.has_value());
    REQUIRE(again.has_value());
    CHECK(first->ptr == again->ptr);
    CHECK(record->views->size() == 1);

    const auto mip =
        record->views->render_target(*record->resource.get(), record->description, { .texture = *target, .mip = 1 });
    REQUIRE(mip.has_value());
    CHECK(mip->ptr != first->ptr);
    CHECK(record->views->size() == 2);

    const auto depth = resources.create_texture(
        { .width = 64,
          .height = 64,
          .format = rhi::format::d32_float,
          .usage = rhi::texture_usage::depth_stencil | rhi::texture_usage::shader_resource,
          .debug_name = "depth" });
    REQUIRE(depth.has_value());
    const rhi::d3d12::texture_record* depth_record = manager.find(*depth);
    REQUIRE(depth_record != nullptr);
    REQUIRE(depth_record->views != nullptr);
    CHECK(depth_record->views
              ->depth_stencil(*depth_record->resource.get(), depth_record->description, { .texture = *depth })
              .has_value());
}
