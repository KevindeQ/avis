// Building render graphs (docs/tasks/09-render-graph.md): virtual resources, descriptions and usages (§9.2).

#include "avis/graphics/buffer.h"
#include "avis/graphics/resource.h"
#include "avis/graphics/texture.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>

using namespace avis::graphics;
using avis::rhi::access_state;
using avis::rhi::pipeline_synchronization;
using avis::rhi::queue_type;
using avis::rhi::resource_access;
using avis::rhi::texture_layout;

namespace
{
    constexpr bool same_state(const access_state& actual, const access_state& expected) noexcept
    {
        return actual.synchronization == expected.synchronization && actual.access == expected.access &&
               actual.layout == expected.layout;
    }

    // Buffers have no layout
    constexpr bool same_buffer_state(const access_state& actual, const access_state& expected) noexcept
    {
        return actual.synchronization == expected.synchronization && actual.access == expected.access;
    }

    constexpr pass_type graphics_pass = pass_type::graphics;
    constexpr pass_type compute_pass = pass_type::compute;
    constexpr pass_type copy_pass = pass_type::copy;
    constexpr queue_type graphics_queue = queue_type::graphics;
    constexpr queue_type compute_queue = queue_type::compute;
    constexpr queue_type copy_queue = queue_type::copy;

    // The rows of 10 §6.3, texture usages
    static_assert(same_state(
        usage_access_state(texture_write::render_target, graphics_pass, graphics_queue),
        { pipeline_synchronization::render_target, resource_access::render_target, texture_layout::render_target }));
    static_assert(same_state(
        usage_access_state(texture_write::depth_write, graphics_pass, graphics_queue),
        { pipeline_synchronization::depth_stencil, resource_access::depth_write, texture_layout::depth_write }));
    static_assert(same_state(
        usage_access_state(texture_read::depth_read, graphics_pass, graphics_queue),
        { pipeline_synchronization::depth_stencil | pipeline_synchronization::pixel_shading,
          resource_access::depth_read | resource_access::shader_resource,
          texture_layout::depth_read }));
    static_assert(same_state(
        usage_access_state(texture_read::depth_read, compute_pass, graphics_queue),
        { pipeline_synchronization::depth_stencil | pipeline_synchronization::compute_shading,
          resource_access::depth_read | resource_access::shader_resource,
          texture_layout::depth_read }));
    static_assert(same_state(
        usage_access_state(texture_read::srv_pixel, graphics_pass, graphics_queue),
        { pipeline_synchronization::pixel_shading,
          resource_access::shader_resource,
          texture_layout::shader_resource }));
    static_assert(same_state(
        usage_access_state(texture_read::srv_compute, compute_pass, compute_queue),
        { pipeline_synchronization::compute_shading,
          resource_access::shader_resource,
          texture_layout::shader_resource }));
    static_assert(same_state(
        usage_access_state(texture_write::uav, compute_pass, compute_queue),
        { pipeline_synchronization::compute_shading,
          resource_access::unordered_access,
          texture_layout::unordered_access }));
    static_assert(same_state(
        usage_access_state(texture_write::uav, graphics_pass, graphics_queue),
        { pipeline_synchronization::pixel_shading,
          resource_access::unordered_access,
          texture_layout::unordered_access }));
    static_assert(same_state(
        usage_access_state(texture_read::copy_source, copy_pass, graphics_queue),
        { pipeline_synchronization::copy, resource_access::copy_source, texture_layout::copy_source }));
    static_assert(same_state(
        usage_access_state(texture_write::copy_destination, copy_pass, graphics_queue),
        { pipeline_synchronization::copy, resource_access::copy_destination, texture_layout::copy_destination }));

    // Copies on the copy queue use the common layout
    static_assert(same_state(
        usage_access_state(texture_read::copy_source, copy_pass, copy_queue),
        { pipeline_synchronization::copy, resource_access::copy_source, texture_layout::common }));
    static_assert(same_state(
        usage_access_state(texture_write::copy_destination, copy_pass, copy_queue),
        { pipeline_synchronization::copy, resource_access::copy_destination, texture_layout::common }));

    // The rows of 10 §6.3, buffer usages
    static_assert(same_buffer_state(
        usage_access_state(buffer_read::srv_pixel, graphics_pass, graphics_queue),
        { pipeline_synchronization::pixel_shading, resource_access::shader_resource }));
    static_assert(same_buffer_state(
        usage_access_state(buffer_read::srv_compute, compute_pass, compute_queue),
        { pipeline_synchronization::compute_shading, resource_access::shader_resource }));
    static_assert(same_buffer_state(
        usage_access_state(buffer_read::indirect_arguments, graphics_pass, graphics_queue),
        { pipeline_synchronization::execute_indirect, resource_access::indirect_argument }));
    static_assert(same_buffer_state(
        usage_access_state(buffer_write::uav, compute_pass, compute_queue),
        { pipeline_synchronization::compute_shading, resource_access::unordered_access }));
    static_assert(same_buffer_state(
        usage_access_state(buffer_write::uav, graphics_pass, graphics_queue),
        { pipeline_synchronization::pixel_shading, resource_access::unordered_access }));
    static_assert(same_buffer_state(
        usage_access_state(buffer_read::copy_source, copy_pass, copy_queue),
        { pipeline_synchronization::copy, resource_access::copy_source }));
    static_assert(same_buffer_state(
        usage_access_state(buffer_write::copy_destination, copy_pass, copy_queue),
        { pipeline_synchronization::copy, resource_access::copy_destination }));

    // Relative sizes round up and are never 0
    static_assert(view_relative(0.5f).resolve(1920, 1080) == texture_extent{ 960, 540 });
    static_assert(view_relative(0.5f).resolve(1921, 1081) == texture_extent{ 961, 541 });
    static_assert(view_relative(0.5f).resolve(1, 1) == texture_extent{ 1, 1 });
    static_assert(view_relative(0.5f).resolve(0, 0) == texture_extent{ 1, 1 });
    static_assert(view_relative(0.25f).resolve(1023, 7) == texture_extent{ 256, 2 });
    static_assert(view_relative(1.0f).resolve(1280, 720) == texture_extent{ 1280, 720 });
    static_assert(absolute(64, 32).resolve(1920, 1080) == texture_extent{ 64, 32 });

    // Handles: a default one is invalid, and textures and buffers don't convert into each other
    static_assert(!resource{}.valid());
    static_assert(!texture{}.valid());
    static_assert(!buffer{}.valid());
    static_assert(std::is_convertible_v<texture, resource>);
    static_assert(std::is_convertible_v<buffer, resource>);
    static_assert(!std::is_convertible_v<texture, buffer>);
    static_assert(!std::is_convertible_v<buffer, texture>);
    static_assert(!std::is_constructible_v<texture, std::uint32_t, std::uint32_t, std::uint32_t>);
} // namespace

TEST_CASE("render graph usages map to the access states of 10 §6.3", "[render_graph][unit]")
{
    // The checks are the static_asserts above; a runtime call for coverage of the non-constant path
    const access_state state = usage_access_state(texture_write::uav, pass_type::compute, queue_type::compute);
    CHECK(state.layout == texture_layout::unordered_access);
}

TEST_CASE("view_relative(0.5) resolves to half the view size, rounded up, never 0", "[render_graph][unit]")
{
    const texture_size half = view_relative(0.5f);
    CHECK(half.mode == size_mode::view_relative);

    for (std::uint32_t extent = 0; extent < 4096; ++extent)
    {
        const texture_extent resolved = half.resolve(extent, extent + 1);
        CHECK(resolved.width == (extent == 0 ? 1 : (extent + 1) / 2));
        CHECK(resolved.height == (extent + 2) / 2);
    }

    const texture_description description{ .size = view_relative(0.5f), .format = avis::rhi::format::r8_unorm };
    CHECK(description.size.resolve(1920, 1080) == texture_extent{ 960, 540 });
    CHECK(description.mip_levels == 1);
    CHECK_FALSE(description.clear_value.has_value());
}

TEST_CASE("render graph handles carry an index and a version", "[render_graph][unit]")
{
    using avis::graphics::detail::resource_handles;

    const texture made = resource_handles::make<texture>(3, 2, 7);
    CHECK(made.valid());
    CHECK(made.index() == 3);
    CHECK(made.version() == 2);
#ifndef NDEBUG
    CHECK(made.frame_serial() == 7);
#endif

    CHECK(made == resource_handles::make<texture>(3, 2, 7));
    CHECK_FALSE(made == resource_handles::make<texture>(3, 3, 7));

    const buffer default_buffer;
    CHECK_FALSE(default_buffer.valid());
    CHECK(default_buffer.index() == resource::invalid_index);
}
