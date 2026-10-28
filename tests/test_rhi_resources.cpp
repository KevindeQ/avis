// Resources, placed resources and memory (docs/tasks/06-rhi.md §6.7). Needs a D3D12 device (WARP is enough).

#include "helpers/recorded_assertions.h"
#include "helpers/rhi_fixture.h"

#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/native_access.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/error.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/resources.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

using namespace avis;
using rhi::buffer_usage;
using rhi::format;
using rhi::texture_usage;

namespace
{
    constexpr std::uint64_t sixty_four_kib = 64 * 1024;
} // namespace

TEST_CASE("Textures get the persistent views their usage asks for", "[rhi]")
{
    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    hardware_interface.accept_errors(); // the checks of unrequested views log
    rhi::resources resources{ *hardware_interface };

    struct texture_case
    {
        const char* name;
        rhi::texture_description description;
    };
    const texture_case cases[] = {
        { "sampled",
          { .width = 64, .height = 64, .format = format::rgba8_unorm_srgb, .usage = texture_usage::shader_resource } },
        { "storage",
          { .width = 64,
            .height = 64,
            .format = format::rgba16_float,
            .usage = texture_usage::shader_resource | texture_usage::unordered_access } },
        { "render target",
          { .width = 64,
            .height = 64,
            .format = format::rgba16_float,
            .usage = texture_usage::render_target | texture_usage::shader_resource,
            .optimized_clear = rhi::clear_value{} } },
        { "depth",
          { .width = 64,
            .height = 64,
            .format = format::d32_float,
            .usage = texture_usage::depth_stencil | texture_usage::shader_resource,
            .optimized_clear = rhi::clear_value{},
            .initial_layout = rhi::texture_layout::depth_write } },
        { "render target and storage",
          { .width = 64,
            .height = 64,
            .format = format::r11g11b10_float,
            .usage =
                texture_usage::render_target | texture_usage::shader_resource | texture_usage::unordered_access } },
        { "compressed upload target",
          { .width = 64,
            .height = 64,
            .format = format::bc7_unorm_srgb,
            .usage = texture_usage::shader_resource | texture_usage::copy_destination } },
        { "array",
          { .width = 32,
            .height = 32,
            .depth_or_layers = 4,
            .format = format::r16_float,
            .usage = texture_usage::shader_resource } },
        { "volume",
          { .dimension = rhi::texture_dimension::texture_3d,
            .width = 16,
            .height = 16,
            .depth_or_layers = 16,
            .format = format::r32_float,
            .usage = texture_usage::shader_resource | texture_usage::unordered_access } },
    };

    for (const texture_case& entry : cases)
    {
        INFO(entry.name);
        const auto texture = resources.create_texture(entry.description);
        REQUIRE(texture.has_value());
        const bool sampled = entry.description.usage.count(texture_usage::shader_resource) != 0;
        const bool storage = entry.description.usage.count(texture_usage::unordered_access) != 0;

        assertions.clear();
        CHECK((resources.srv(*texture).value != 0) == sampled);
        CHECK((resources.uav(*texture).value != 0) == storage);
#ifndef NDEBUG
        CHECK(assertions.count() == (sampled ? 0u : 1u) + (storage ? 0u : 1u));
#endif
        CHECK(resources.description(*texture).width == entry.description.width);
        CHECK(resources.description(*texture).debug_name.empty());
        resources.destroy(*texture);
    }
}

TEST_CASE("Buffers get views, addresses and mapped memory from their usage", "[rhi]")
{
    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    hardware_interface.accept_errors();
    rhi::resources resources{ *hardware_interface };

    const auto structured = resources.create_buffer(
        { .size = 1024, .structure_stride = 16, .usage = buffer_usage::shader_resource, .debug_name = "instances" });
    const auto raw = resources.create_buffer(
        { .size = 1024, .usage = buffer_usage::shader_resource | buffer_usage::unordered_access, .debug_name = "raw" });
    const auto indices = resources.create_buffer(
        { .size = 600, .usage = buffer_usage::index | buffer_usage::copy_destination, .debug_name = "indices" });
    const auto arguments = resources.create_buffer(
        { .size = 64,
          .usage = buffer_usage::indirect_arguments | buffer_usage::unordered_access,
          .debug_name = "arguments" });
    const auto constants =
        resources.create_buffer({ .size = 100, .usage = buffer_usage::constant, .debug_name = "constants" });
    const auto upload = resources.create_buffer(
        { .size = 4096,
          .usage = buffer_usage::copy_source,
          .memory = rhi::memory_type::upload,
          .debug_name = "upload" });
    const auto readback = resources.create_buffer(
        { .size = 4096,
          .usage = buffer_usage::copy_destination,
          .memory = rhi::memory_type::readback,
          .debug_name = "readback" });
    for (const auto* buffer : { &structured, &raw, &indices, &arguments, &constants, &upload, &readback })
    {
        REQUIRE(buffer->has_value());
        CHECK(resources.buffer_address(**buffer).value != 0);
    }

    CHECK(resources.srv(*structured).value != 0);
    CHECK(resources.srv(*raw).value != 0);
    CHECK(resources.uav(*raw).value != 0);
    CHECK(resources.uav(*arguments).value != 0);
    assertions.clear();
    CHECK(resources.uav(*structured).value == 0);
    CHECK(resources.srv(*indices).value == 0);
#ifndef NDEBUG
    CHECK(assertions.count() == 2);
#endif

    // Constant buffers are rounded up to 256 bytes
    CHECK(rhi::d3d12::native_resource(*hardware_interface, *constants)->GetDesc().Width == 256);

    // Upload memory is mapped for the buffer's lifetime and writable
    const std::span<std::byte> bytes = resources.mapped_bytes(*upload);
    REQUIRE(bytes.size() == 4096);
    std::ranges::fill(bytes, std::byte{ 0x5A });
    CHECK(bytes[4095] == std::byte{ 0x5A });
    CHECK(resources.mapped_bytes(*readback).size() == 4096);
    assertions.clear();
    CHECK(resources.mapped_bytes(*raw).empty());
#ifndef NDEBUG
    CHECK(assertions.count() == 1);
#endif

    const auto sampler =
        resources.create_sampler({ .minification = rhi::filter::anisotropic, .maximum_anisotropy = 8 });
    REQUIRE(sampler.has_value());
    CHECK(resources.sampler_index(*sampler).value != 0);
}

TEST_CASE("A full mip chain gets one UAV per mip, and cubes count six faces each", "[rhi]")
{
    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    hardware_interface.accept_errors();
    rhi::resources resources{ *hardware_interface };

    const auto chain = resources.create_texture(
        { .width = 256,
          .height = 128,
          .mip_levels = 0,
          .format = format::rgba8_unorm,
          .usage = texture_usage::unordered_access });
    REQUIRE(chain.has_value());
    CHECK(rhi::d3d12::native_resource(*hardware_interface, *chain)->GetDesc().MipLevels == 9);
    for (std::uint32_t mip = 0; mip < 9; ++mip)
    {
        CHECK(resources.uav(*chain, mip).value != 0);
    }
    CHECK(resources.uav(*chain, 0) != resources.uav(*chain, 8));
    CHECK(resources.uav(*chain, 9).value == 0);
#ifndef NDEBUG
    CHECK(assertions.contains("mip out of range"));
#endif

    const auto cubes = resources.create_texture(
        { .dimension = rhi::texture_dimension::cube,
          .width = 32,
          .height = 32,
          .depth_or_layers = 2,
          .format = format::rgba16_float,
          .usage = texture_usage::shader_resource });
    REQUIRE(cubes.has_value());
    CHECK(rhi::d3d12::native_resource(*hardware_interface, *cubes)->GetDesc().DepthOrArraySize == 12);
}

TEST_CASE("Unsupported descriptions fail with invalid_argument", "[rhi]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };

    const auto compressed_target = resources.create_texture(
        { .width = 64, .height = 64, .format = format::bc1_unorm, .usage = texture_usage::render_target });
    REQUIRE_FALSE(compressed_target.has_value());
    CHECK(rhi::is_error(compressed_target.error(), rhi::error_kind::invalid_argument));
    CHECK(compressed_target.error().message.contains("bc1_unorm"));
    CHECK(compressed_target.error().message.contains("render_target"));

    const auto srgb_storage =
        resources.create_texture({ .format = format::rgba8_unorm_srgb, .usage = texture_usage::unordered_access });
    REQUIRE_FALSE(srgb_storage.has_value());
    CHECK(rhi::is_error(srgb_storage.error(), rhi::error_kind::invalid_argument));

    const auto empty = resources.create_buffer({ .size = 0, .usage = buffer_usage::copy_source });
    REQUIRE_FALSE(empty.has_value());
    CHECK(rhi::is_error(empty.error(), rhi::error_kind::invalid_argument));

    const auto writable_upload = resources.create_buffer(
        { .size = 256, .usage = buffer_usage::unordered_access, .memory = rhi::memory_type::upload });
    REQUIRE_FALSE(writable_upload.has_value());
    CHECK(rhi::is_error(writable_upload.error(), rhi::error_kind::invalid_argument));
}

TEST_CASE("A destroyed resource is freed only after poll_completions", "[rhi]")
{
    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    hardware_interface.accept_errors(); // the second destroy asserts
    rhi::resources resources{ *hardware_interface };
    rhi::d3d12::resource_manager& manager = hardware_interface->state().resources();
    const rhi::texture_description description{ .width = 16, .height = 16, .usage = texture_usage::shader_resource };

    const auto texture = resources.create_texture(description);
    REQUIRE(texture.has_value());
    const rhi::bindless_index index = resources.srv(*texture);

    resources.destroy(*texture);
    CHECK(manager.find(*texture) == nullptr);
    CHECK(rhi::d3d12::native_resource(*hardware_interface, *texture) == nullptr);

    assertions.clear();
    resources.destroy(*texture); // a no-op
#ifndef NDEBUG
    CHECK(assertions.contains("stale or invalid texture handle"));
#endif

    // Not freed yet: its index isn't back on the free list
    const auto before_poll = resources.create_texture(description);
    REQUIRE(before_poll.has_value());
    CHECK(resources.srv(*before_poll) != index);

    rhi::completion_driver{ *hardware_interface }.poll_completions();
    const auto after_poll = resources.create_texture(description);
    REQUIRE(after_poll.has_value());
    CHECK(resources.srv(*after_poll) == index);
}

TEST_CASE("Placed resources alias in a heap, and the heap memory outlives them", "[rhi]")
{
    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    rhi::memory memory{ *hardware_interface };
    rhi::d3d12::resource_manager& manager = hardware_interface->state().resources();

    const rhi::texture_description first_description{ .width = 256,
                                                      .height = 256,
                                                      .format = format::rgba8_unorm,
                                                      .usage =
                                                          texture_usage::render_target | texture_usage::shader_resource,
                                                      .debug_name = "first alias" };
    const rhi::texture_description second_description{ .width = 128,
                                                       .height = 128,
                                                       .format = format::rgba16_float,
                                                       .usage = texture_usage::unordered_access,
                                                       .debug_name = "second alias" };
    const rhi::allocation_requirements requirements = memory.requirements(first_description);
    CHECK(requirements.alignment == sixty_four_kib);

    const auto heap = memory.create_heap({ .size = 4 * 1024 * 1024, .debug_name = "transient heap" });
    REQUIRE(heap.has_value());
    const auto first = memory.create_placed_texture(*heap, 0, first_description);
    const auto second = memory.create_placed_texture(*heap, 0, second_description);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const auto buffer = memory.create_placed_buffer(
        *heap,
        sixty_four_kib,
        { .size = 1024, .usage = buffer_usage::unordered_access, .debug_name = "placed buffer" });
    REQUIRE(buffer.has_value());
    CHECK(resources.buffer_address(*buffer).value != 0);

    const rhi::d3d12::texture_record* const first_record = manager.find(*first);
    const rhi::d3d12::texture_record* const second_record = manager.find(*second);
    REQUIRE(first_record != nullptr);
    REQUIRE(second_record != nullptr);
    CHECK(first_record->placed);
    CHECK(first_record->heap == second_record->heap); // the same memory, at the same offset
    CHECK(first_record->heap->live_resources == 3);
    CHECK(first_record->views != nullptr);
    const std::weak_ptr<rhi::d3d12::placed_heap> memory_alive = first_record->heap;

    SECTION("destroying the resources, then the heap, frees the memory once")
    {
        resources.destroy(*first);
        resources.destroy(*second);
        resources.destroy(*buffer);
        memory.destroy(*heap);
        CHECK(assertions.count() == 0);
        CHECK_FALSE(memory_alive.expired());

        hardware_interface->wait_idle();
        CHECK(memory_alive.expired());
    }

    SECTION("destroying the heap first asserts, and the memory still outlives the resources")
    {
        hardware_interface.accept_errors();
        memory.destroy(*heap);
#ifndef NDEBUG
        CHECK(assertions.contains("live placed resource"));
#endif
        hardware_interface->wait_idle();
        CHECK_FALSE(memory_alive.expired()); // the placed records still hold it
        CHECK(manager.find(*first) != nullptr);

        resources.destroy(*first);
        resources.destroy(*second);
        resources.destroy(*buffer);
        hardware_interface->wait_idle();
        CHECK(memory_alive.expired());
    }

    SECTION("a stale heap handle is invalid_handle")
    {
        resources.destroy(*first);
        resources.destroy(*second);
        resources.destroy(*buffer);
        memory.destroy(*heap);
        const auto placed = memory.create_placed_texture(*heap, 0, first_description);
        REQUIRE_FALSE(placed.has_value());
        CHECK(rhi::is_error(placed.error(), rhi::error_kind::invalid_handle));
    }

    SECTION("a misaligned offset is invalid_argument")
    {
        const auto placed = memory.create_placed_texture(*heap, 256, first_description);
        REQUIRE_FALSE(placed.has_value());
        CHECK(rhi::is_error(placed.error(), rhi::error_kind::invalid_argument));
    }
}

TEST_CASE("Requirements and copy footprints", "[rhi]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    rhi::memory memory{ *hardware_interface };

    CHECK(memory.requirements(rhi::texture_description{ .width = 100, .height = 100 }).alignment == sixty_four_kib);
    CHECK(
        memory.requirements(rhi::buffer_description{ .size = 100, .usage = buffer_usage::copy_source }).alignment ==
        sixty_four_kib);

    const auto plain =
        resources.create_texture({ .width = 100, .height = 50, .mip_levels = 2, .format = format::rgba8_unorm });
    REQUIRE(plain.has_value());
    const rhi::copy_footprint top = resources.copyable_footprint(*plain, { .mip = 0 });
    CHECK(top.width == 100);
    CHECK(top.height == 50);
    CHECK(top.row_size == 400);
    CHECK(top.row_pitch == 512); // 256-byte aligned
    CHECK(top.row_count == 50);
    CHECK(top.total_size == 49 * 512 + 400);
    const rhi::copy_footprint second = resources.copyable_footprint(*plain, { .mip = 1 });
    CHECK(second.width == 50);
    CHECK(second.row_pitch % 256 == 0);

    const auto compressed = resources.create_texture(
        { .width = 64, .height = 64, .format = format::bc1_unorm, .usage = texture_usage::shader_resource });
    REQUIRE(compressed.has_value());
    const rhi::copy_footprint blocks = resources.copyable_footprint(*compressed, {});
    CHECK(blocks.row_count == 16); // block rows
    CHECK(blocks.row_size == 16 * 8);
    CHECK(blocks.row_pitch == 256);
}

TEST_CASE("local_memory_budget reports a budget", "[rhi]")
{
    tests::test_rhi hardware_interface;
    const rhi::memory_budget budget = hardware_interface->local_memory_budget();
    CHECK(budget.budget > 0);
}
