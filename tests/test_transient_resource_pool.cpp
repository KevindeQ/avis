// The render graph's transient resource pool (docs/tasks/09-render-graph.md §9.7): heaps per frame context, the
// placed-resource cache, growth and shrinking, and aliasing on the GPU.

#include "helpers/rhi_fixture.h"

#include "avis/graphics/transient_resource_pool.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using avis::graphics::transient_resource_pool;
using avis::tests::test_rhi;
namespace rhi = avis::rhi;

namespace
{
    constexpr std::uint64_t mebibyte = std::uint64_t{ 1 } << 20;

    std::uint64_t align_up(const std::uint64_t value, const std::uint64_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    std::weak_ptr<rhi::d3d12::placed_heap> heap_memory(test_rhi& hardware_interface, const rhi::heap_handle heap)
    {
        const rhi::d3d12::heap_record* const record = hardware_interface->state().resources().find(heap);
        REQUIRE(record != nullptr);
        return record->heap;
    }
} // namespace

TEST_CASE("transient pool: a steady-state frame creates no placed resources", "[render_graph]")
{
    test_rhi hardware_interface;
    rhi::d3d12::resource_manager& manager = hardware_interface->state().resources();
    constexpr std::uint32_t frames_in_flight = 2;
    transient_resource_pool pool{ rhi::memory{ *hardware_interface },
                                  rhi::resources{ *hardware_interface },
                                  frames_in_flight };

    const rhi::texture_description color{ .width = 256,
                                          .height = 256,
                                          .format = rhi::format::rgba8_unorm,
                                          .usage =
                                              rhi::texture_usage::render_target | rhi::texture_usage::shader_resource,
                                          .initial_layout = rhi::texture_layout::undefined,
                                          .debug_name = "color" };
    const rhi::texture_description occlusion{ .width = 128,
                                              .height = 128,
                                              .format = rhi::format::r8_unorm,
                                              .usage = rhi::texture_usage::unordered_access,
                                              .initial_layout = rhi::texture_layout::undefined,
                                              .debug_name = "occlusion" };
    const rhi::buffer_description counters{ .size = 4096,
                                            .usage = rhi::buffer_usage::unordered_access,
                                            .debug_name = "counters" };

    const rhi::allocation_requirements color_size = pool.requirements(color);
    const rhi::allocation_requirements occlusion_size = pool.requirements(occlusion);
    const rhi::allocation_requirements counters_size = pool.requirements(counters);
    const std::uint64_t occlusion_offset = align_up(color_size.size, occlusion_size.alignment);
    const std::uint64_t counters_offset = align_up(occlusion_offset + occlusion_size.size, counters_size.alignment);
    const std::uint64_t heap_size = counters_offset + counters_size.size;

    std::array<rhi::texture_handle, frames_in_flight> first_color{};
    std::uint32_t texture_slots = 0;
    std::uint32_t buffer_slots = 0;
    for (std::uint32_t frame = 0; frame < 10; ++frame)
    {
        const std::uint32_t context = frame % frames_in_flight;
        REQUIRE(pool.prepare(context, heap_size).has_value());
        const auto color_texture = pool.acquire_texture(context, color, 0);
        const auto occlusion_texture = pool.acquire_texture(context, occlusion, occlusion_offset);
        const auto counters_buffer = pool.acquire_buffer(context, counters, counters_offset);
        REQUIRE(color_texture.has_value());
        REQUIRE(occlusion_texture.has_value());
        REQUIRE(counters_buffer.has_value());

        if (frame < frames_in_flight)
        {
            first_color[context] = *color_texture;
        }
        else
        {
            CHECK(*color_texture == first_color[context]); // the cached one
        }
        if (frame == frames_in_flight - 1)
        {
            texture_slots = manager.texture_slots_used();
            buffer_slots = manager.buffer_slots_used();
        }
    }

    CHECK(pool.statistics().textures_created == 2 * frames_in_flight);
    CHECK(pool.statistics().buffers_created == frames_in_flight);
    CHECK(pool.statistics().heaps_created == frames_in_flight);
    CHECK(pool.statistics().resources_destroyed == 0);
    CHECK(manager.texture_slots_used() == texture_slots);
    CHECK(manager.buffer_slots_used() == buffer_slots);
    CHECK(pool.heap_size(0) == transient_resource_pool::heap_granularity);
    CHECK(pool.heap(0) != pool.heap(1));

    // A debug name that changes doesn't make a new resource
    rhi::texture_description renamed = color;
    renamed.debug_name = "main/color";
    REQUIRE(pool.prepare(0, heap_size).has_value());
    const auto again = pool.acquire_texture(0, renamed, 0);
    REQUIRE(again.has_value());
    CHECK(*again == first_color[0]);
}

TEST_CASE("transient pool: heaps grow in 64 MiB steps and shrink after 120 small frames", "[render_graph]")
{
    test_rhi hardware_interface;
    transient_resource_pool pool{ rhi::memory{ *hardware_interface }, rhi::resources{ *hardware_interface }, 2 };
    const rhi::texture_description target{ .width = 64,
                                           .height = 64,
                                           .usage = rhi::texture_usage::render_target,
                                           .initial_layout = rhi::texture_layout::undefined };

    REQUIRE(pool.prepare(0, mebibyte).has_value());
    CHECK(pool.heap_size(0) == 64 * mebibyte);
    CHECK(pool.heap_size(1) == 0);
    REQUIRE(pool.acquire_texture(0, target, 0).has_value());
    const std::weak_ptr<rhi::d3d12::placed_heap> first_heap = heap_memory(hardware_interface, pool.heap(0));

    SECTION("growing replaces the heap; the old one is freed once the GPU is done with it")
    {
        REQUIRE(pool.prepare(0, 65 * mebibyte).has_value());
        CHECK(pool.heap_size(0) == 128 * mebibyte);
        CHECK(pool.high_water_mark(0) == 65 * mebibyte);
        CHECK(pool.statistics().heaps_created == 2);
        CHECK(pool.statistics().heaps_destroyed == 1);
        CHECK(pool.statistics().resources_destroyed == 1); // the cached texture went with its heap
        CHECK_FALSE(first_heap.expired()); // deferred: released by poll_completions

        hardware_interface->wait_idle(); // runs poll_completions
        CHECK(first_heap.expired());

        // A placed resource of the new heap is a new one
        REQUIRE(pool.acquire_texture(0, target, 0).has_value());
        CHECK(pool.statistics().textures_created == 2);
    }

    SECTION("120 frames below the size shrink the heap to what they needed")
    {
        REQUIRE(pool.prepare(0, 129 * mebibyte).has_value());
        CHECK(pool.heap_size(0) == 192 * mebibyte);
        for (std::uint32_t frame = 1; frame < transient_resource_pool::retention_frames; ++frame)
        {
            REQUIRE(pool.prepare(0, frame == 50 ? 70 * mebibyte : 2 * mebibyte).has_value());
            CHECK(pool.heap_size(0) == 192 * mebibyte);
        }
        REQUIRE(pool.prepare(0, 2 * mebibyte).has_value());
        CHECK(pool.heap_size(0) == 128 * mebibyte); // 70 MiB, the largest of those frames, rounded up
        CHECK(pool.high_water_mark(0) == 129 * mebibyte);
        hardware_interface->wait_idle();
    }
}

TEST_CASE("transient pool: resources unused for 120 frames are destroyed", "[render_graph]")
{
    test_rhi hardware_interface;
    transient_resource_pool pool{ rhi::memory{ *hardware_interface }, rhi::resources{ *hardware_interface }, 1 };
    const rhi::buffer_description scratch{ .size = 1024, .usage = rhi::buffer_usage::unordered_access };

    REQUIRE(pool.prepare(0, mebibyte).has_value());
    REQUIRE(pool.acquire_buffer(0, scratch, 0).has_value());
    for (std::uint32_t frame = 0; frame < transient_resource_pool::retention_frames; ++frame)
    {
        REQUIRE(pool.prepare(0, mebibyte).has_value());
    }
    CHECK(pool.statistics().resources_destroyed == 0);
    REQUIRE(pool.prepare(0, mebibyte).has_value());
    CHECK(pool.statistics().resources_destroyed == 1);
}

TEST_CASE("transient pool: two resources at one offset alias", "[render_graph]")
{
    test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    transient_resource_pool pool{ rhi::memory{ *hardware_interface }, resources, 1 };
    constexpr std::uint64_t size = 4096;

    // Two descriptions, so two cached resources, placed at the same offset
    const rhi::buffer_description writer{ .size = size, .usage = rhi::buffer_usage::copy_destination };
    const rhi::buffer_description reader{ .size = size, .usage = rhi::buffer_usage::copy_source };
    REQUIRE(pool.prepare(0, mebibyte).has_value());
    const auto first = pool.acquire_buffer(0, writer, 0);
    const auto second = pool.acquire_buffer(0, reader, 0);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first != *second);

    const auto upload = resources.create_buffer(
        { .size = size, .usage = rhi::buffer_usage::copy_source, .memory = rhi::memory_type::upload });
    const auto readback = resources.create_buffer(
        { .size = size, .usage = rhi::buffer_usage::copy_destination, .memory = rhi::memory_type::readback });
    REQUIRE(upload.has_value());
    REQUIRE(readback.has_value());
    std::vector<std::uint32_t> pattern(size / sizeof(std::uint32_t));
    for (std::size_t index = 0; index < pattern.size(); ++index)
    {
        pattern[index] = static_cast<std::uint32_t>(index * 2654435761u);
    }
    std::memcpy(resources.mapped_bytes(*upload).data(), pattern.data(), size);

    auto list = rhi::command_lists{ *hardware_interface }.acquire<rhi::queue_type::graphics>("aliasing test");
    REQUIRE(list.has_value());
    list->copy_buffer(*first, 0, *upload, 0, size);
    // Aliasing: the copy into the first buffer completes before the second one's memory is read
    const rhi::global_barrier aliasing{
        .before = { rhi::pipeline_synchronization::copy, rhi::resource_access::copy_destination },
        .after = { rhi::pipeline_synchronization::copy, rhi::resource_access::copy_source }
    };
    list->barrier({ &aliasing, 1 }, {}, {});
    list->copy_buffer(*readback, 0, *second, 0, size);

    std::array lists{ std::move(*list) };
    const auto point = rhi::queue<rhi::queue_type::graphics>{ *hardware_interface }.submit(lists);
    REQUIRE(point.has_value());
    hardware_interface.wait_for(*point);

    std::vector<std::uint32_t> read(pattern.size());
    std::memcpy(read.data(), resources.mapped_bytes(*readback).data(), size);
    CHECK(read == pattern);

    resources.destroy(*upload);
    resources.destroy(*readback);
}
