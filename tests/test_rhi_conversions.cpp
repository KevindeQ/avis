// Public RHI types → D3D12 and HRESULT → avis::error (docs/tasks/06-rhi.md §6.3). No device. The lookup tables are
// covered by static_asserts in their .cpp files; these checks exercise the functions and the rules on top.

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/conversions/barriers.h"
#include "avis/rhi/d3d12/conversions/formats.h"
#include "avis/rhi/d3d12/conversions/pipelines.h"
#include "avis/rhi/d3d12/conversions/queues.h"
#include "avis/rhi/d3d12/conversions/resources.h"
#include "avis/rhi/d3d12/conversions/samplers.h"
#include "avis/rhi/error.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <string>

namespace
{
    using namespace avis::rhi;
    using namespace avis::rhi::d3d12;
} // namespace

TEST_CASE("formats and queues map to their D3D12 values", "[rhi][unit]")
{
    CHECK(to_native(format::unknown) == DXGI_FORMAT_UNKNOWN);
    CHECK(to_native(format::rgba8_unorm_srgb) == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    CHECK(to_native(format::r11g11b10_float) == DXGI_FORMAT_R11G11B10_FLOAT);
    CHECK(to_native(format::d32_float) == DXGI_FORMAT_D32_FLOAT);
    CHECK(to_native(format::bc7_unorm_srgb) == DXGI_FORMAT_BC7_UNORM_SRGB);
    CHECK(to_native(static_cast<format>(200)) == DXGI_FORMAT_UNKNOWN);

    CHECK(to_native_typeless(format::d32_float) == DXGI_FORMAT_R32_TYPELESS);
    CHECK(to_native_depth_view(format::d32_float) == DXGI_FORMAT_R32_FLOAT);
    CHECK(to_native_typeless(format::rgba16_float) == DXGI_FORMAT_R16G16B16A16_FLOAT);

    CHECK(to_native(queue_type::graphics) == D3D12_COMMAND_LIST_TYPE_DIRECT);
    CHECK(to_native(queue_type::compute) == D3D12_COMMAND_LIST_TYPE_COMPUTE);
    CHECK(to_native(queue_type::copy) == D3D12_COMMAND_LIST_TYPE_COPY);
}

TEST_CASE("barrier types convert by cast and by table", "[rhi][unit]")
{
    CHECK(
        to_native(pipeline_synchronization::pixel_shading | pipeline_synchronization::compute_shading) ==
        (D3D12_BARRIER_SYNC_PIXEL_SHADING | D3D12_BARRIER_SYNC_COMPUTE_SHADING));
    CHECK(to_native(avis::flags<resource_access>(resource_access::no_access)) == D3D12_BARRIER_ACCESS_NO_ACCESS);
    CHECK(to_native(access_states::common.access) == D3D12_BARRIER_ACCESS_COMMON);

    CHECK(to_native(texture_layout::undefined) == D3D12_BARRIER_LAYOUT_UNDEFINED);
    CHECK(to_native(texture_layout::depth_read) == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ);
    CHECK(
        to_native(texture_layout::compute_queue_unordered_access) ==
        D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS);

    texture_description cube;
    cube.dimension = texture_dimension::cube;
    cube.width = 64;
    cube.height = 64;
    cube.depth_or_layers = 2;
    cube.mip_levels = 0; // full chain: 7 mips

    const auto everything = to_native(all_subresources, cube);
    CHECK(everything.IndexOrFirstMipLevel == 0xFFFFFFFF);
    CHECK(everything.NumMipLevels == 0);

    const auto upper_mips = to_native(subresource_range{ .first_mip = 2, .first_layer = 6, .layer_count = 6 }, cube);
    CHECK(upper_mips.IndexOrFirstMipLevel == 2);
    CHECK(upper_mips.NumMipLevels == 5);
    CHECK(upper_mips.FirstArraySlice == 6);
    CHECK(upper_mips.NumArraySlices == 6);
    CHECK(upper_mips.NumPlanes == 1);
}

TEST_CASE("sampler and pipeline enums map to their D3D12 values", "[rhi][unit]")
{
    CHECK(to_native(comparison::greater_equal) == D3D12_COMPARISON_FUNC_GREATER_EQUAL);
    CHECK(to_native(address_mode::mirror) == D3D12_TEXTURE_ADDRESS_MODE_MIRROR);
    CHECK(to_native(address_mode::border) == D3D12_TEXTURE_ADDRESS_MODE_BORDER);

    CHECK(to_native_filter(filter::linear, filter::linear, filter::linear, false) == D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    CHECK(to_native_filter(filter::point, filter::point, filter::point, false) == D3D12_FILTER_MIN_MAG_MIP_POINT);
    CHECK(
        to_native_filter(filter::linear, filter::linear, filter::point, true) ==
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
    CHECK(to_native_filter(filter::anisotropic, filter::linear, filter::linear, false) == D3D12_FILTER_ANISOTROPIC);
    CHECK(
        to_native_filter(filter::anisotropic, filter::anisotropic, filter::anisotropic, true) ==
        D3D12_FILTER_COMPARISON_ANISOTROPIC);

    CHECK(to_native(primitive_topology::triangle_strip) == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    CHECK(to_native(primitive_topology::point_list) == D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    CHECK(to_native_topology_type(primitive_topology::line_strip) == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    CHECK(to_native(shader_visibility::pixel) == D3D12_SHADER_VISIBILITY_PIXEL);
}

TEST_CASE("texture descriptions set resource flags from usages", "[rhi][unit]")
{
    // Every combination of the seven usages: never ALLOW_SIMULTANEOUS_ACCESS
    for (std::uint32_t bits = 0; bits < 128; ++bits)
    {
        texture_description description;
        description.usage = avis::flags<texture_usage>(avis::empty);
        description.usage.set_underlying_value(bits);
        const auto native = to_native(description);
        INFO("usage bits " << bits);
        CHECK((native.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) == 0);
        CHECK(
            ((native.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) ==
            (description.usage.count(texture_usage::unordered_access) != 0));
        CHECK(
            ((native.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) ==
            (description.usage.count(texture_usage::render_target) != 0));
        CHECK(
            ((native.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) ==
            (description.usage.count(texture_usage::depth_stencil) != 0));
    }

    SECTION("a sampled depth texture is typeless, with R32_FLOAT for its SRV")
    {
        texture_description depth;
        depth.format = format::d32_float;
        depth.usage = texture_usage::depth_stencil | texture_usage::shader_resource;
        const auto native = to_native(depth);
        CHECK(native.Format == DXGI_FORMAT_R32_TYPELESS);
        CHECK((native.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0);
        CHECK(to_native_depth_view(depth.format) == DXGI_FORMAT_R32_FLOAT);
    }

    SECTION("a depth-only texture is typed and denies shader resources")
    {
        texture_description depth;
        depth.format = format::d32_float;
        depth.usage = texture_usage::depth_stencil;
        const auto native = to_native(depth);
        CHECK(native.Format == DXGI_FORMAT_D32_FLOAT);
        CHECK((native.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0);
    }

    SECTION("dimensions, cubes and mips")
    {
        texture_description cube;
        cube.dimension = texture_dimension::cube;
        cube.width = 128;
        cube.height = 128;
        cube.depth_or_layers = 2;
        cube.mip_levels = 0;
        const auto native = to_native(cube);
        CHECK(native.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        CHECK(native.DepthOrArraySize == 12);
        CHECK(native.MipLevels == 0);
        CHECK(mip_level_count(cube) == 8);
        CHECK(array_size(cube) == 12);

        texture_description volume;
        volume.dimension = texture_dimension::texture_3d;
        volume.width = 16;
        volume.height = 8;
        volume.depth_or_layers = 32;
        volume.mip_levels = 0;
        CHECK(to_native(volume).DepthOrArraySize == 32);
        CHECK(mip_level_count(volume) == 6);
        CHECK(array_size(volume) == 1);
    }
}

TEST_CASE("buffer descriptions and memory types", "[rhi][unit]")
{
    buffer_description constants;
    constants.size = 100;
    constants.usage = buffer_usage::constant;
    CHECK(to_native(constants).Width == 256);
    CHECK(to_native(constants).Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR);

    buffer_description storage;
    storage.size = 100;
    storage.usage = buffer_usage::unordered_access | buffer_usage::shader_resource;
    CHECK(to_native(storage).Width == 100);
    CHECK(to_native(storage).Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    CHECK(to_native(memory_type::gpu_only) == D3D12_HEAP_TYPE_DEFAULT);
    CHECK(to_native(memory_type::upload) == D3D12_HEAP_TYPE_UPLOAD);
    CHECK(to_native(memory_type::readback) == D3D12_HEAP_TYPE_READBACK);
}

TEST_CASE("HRESULTs become classified RHI errors", "[rhi][unit]")
{
    const auto out_of_memory = check(E_OUTOFMEMORY, "x");
    REQUIRE_FALSE(out_of_memory.has_value());
    CHECK(is_error(out_of_memory.error(), error_kind::out_of_memory));
    CHECK(out_of_memory.error().message.starts_with("x: "));
    CHECK(out_of_memory.error().message.ends_with("(HRESULT 0x8007000E)"));

    CHECK(is_error(make_error(E_INVALIDARG, "CreateThing"), error_kind::invalid_argument));
    CHECK(is_error(make_error(DXGI_ERROR_DEVICE_REMOVED, "Signal"), error_kind::device_lost));
    CHECK(is_error(make_error(DXGI_ERROR_DEVICE_RESET, "Signal"), error_kind::device_lost));
    CHECK(is_error(make_error(DXGI_ERROR_DEVICE_HUNG, "Signal"), error_kind::device_lost));

    const avis::error unknown = make_error(static_cast<HRESULT>(0x887A0042), "Present");
    CHECK(is_error(unknown, error_kind::native_error));
    CHECK(unknown.message.contains("(HRESULT 0x887A0042)"));
    CHECK(unknown.domain == avis::error_domain::rhi);

    CHECK(check(S_OK, "fine").has_value());
    CHECK(is_device_removed_code(DXGI_ERROR_DEVICE_REMOVED));
    CHECK_FALSE(is_device_removed_code(E_FAIL));
}
