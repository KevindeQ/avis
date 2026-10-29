// Command lists, recording and submit (docs/tasks/06-rhi.md §6.10). Needs a D3D12 device (WARP is enough); every test
// runs with the debug layer, and the GPU-based validation ones turn it on as well.

#include "helpers/recorded_assertions.h"
#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"
#include "helpers/runtime_fixture.h"

#include "avis/rhi/command_list.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/command_list_pool.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/error.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/transient.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "rhi_test_compute_shader.h"
#include "rhi_test_pixel_shader.h"
#include "rhi_test_vertex_shader.h"

using namespace avis;
using rhi::buffer_usage;
using rhi::format;
using rhi::queue_type;
using rhi::texture_usage;

// ---- the operation sets are types (D-92) ----

namespace
{
    template<class Recorder>
    concept can_dispatch = requires(Recorder recorder) { recorder.dispatch(1u, 1u, 1u); };

    template<class Recorder>
    concept can_draw = requires(Recorder recorder) { recorder.draw(3u); };

    template<class Queue, class List>
    concept submittable = requires(Queue queue, std::span<List> lists) { queue.submit(lists); };

    template<class List, queue_type Narrower>
    concept narrowable = requires(const List& list) { list.template narrow<Narrower>(); };
} // namespace

static_assert(!can_dispatch<rhi::copy_recorder>);
static_assert(can_dispatch<rhi::compute_recorder>);
static_assert(!can_draw<rhi::compute_recorder>);
static_assert(can_draw<rhi::graphics_recorder>);
static_assert(submittable<rhi::queue<queue_type::graphics>, rhi::graphics_command_list>);
static_assert(!submittable<rhi::queue<queue_type::graphics>, rhi::graphics_recorder>);
static_assert(!submittable<rhi::queue<queue_type::graphics>, rhi::compute_command_list>);
static_assert(narrowable<rhi::graphics_command_list, queue_type::compute>);
static_assert(narrowable<rhi::graphics_command_list, queue_type::copy>);
static_assert(narrowable<rhi::compute_command_list, queue_type::copy>);
static_assert(!narrowable<rhi::compute_command_list, queue_type::graphics>);
static_assert(!narrowable<rhi::copy_command_list, queue_type::compute>);

namespace
{
    template<std::size_t Size>
    std::span<const std::byte> bytes(const unsigned char (&code)[Size]) noexcept
    {
        return std::as_bytes(std::span{ code });
    }

    rhi::render_hardware_interface_configuration with_gpu_validation()
    {
        rhi::render_hardware_interface_configuration configuration;
        configuration.debug.gpu_based_validation = true;
        return configuration;
    }

    template<queue_type Queue>
    rhi::command_list<Queue> acquire(tests::test_rhi& hardware_interface, const std::string_view name = {})
    {
        auto list = rhi::command_lists{ *hardware_interface }.acquire<Queue>(name);
        REQUIRE(list.has_value());
        return std::move(*list);
    }

    template<queue_type Queue>
    rhi::synchronization_point submit(
        tests::test_rhi& hardware_interface,
        rhi::command_list<Queue>& list,
        const std::span<const rhi::synchronization_point> waits = {})
    {
        auto point = rhi::queue<Queue>{ *hardware_interface }.submit(std::span{ &list, 1 }, waits);
        REQUIRE(point.has_value());
        return *point;
    }

    rhi::buffer_handle readback_buffer(rhi::resources& resources, const std::uint64_t size)
    {
        auto buffer = resources.create_buffer(
            { .size = size, .usage = buffer_usage::copy_destination, .memory = rhi::memory_type::readback, .debug_name = "readback" });
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    rhi::buffer_handle upload_buffer(rhi::resources& resources, const std::uint64_t size)
    {
        auto buffer = resources.create_buffer(
            { .size = size, .usage = buffer_usage::copy_source, .memory = rhi::memory_type::upload, .debug_name = "upload" });
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    struct fill_kit
    {
        rhi::root_signature_handle signature;
        rhi::compute_pipeline_handle pipeline;
    };

    fill_kit make_fill(tests::test_rhi& hardware_interface, const std::uint32_t constant_count = 3, const char* name = "fill")
    {
        rhi::pipelines pipelines{ *hardware_interface };
        const std::array parameters{ rhi::root_parameter{ .kind = rhi::root_parameter_kind::constants,
                                                          .shader_register = 0,
                                                          .constant_count = constant_count } };
        auto signature = pipelines.create_root_signature({ .parameters = parameters, .debug_name = name });
        REQUIRE(signature.has_value());
        auto pipeline = pipelines.create_pipeline(rhi::compute_pipeline_description{
            .root_signature = *signature, .compute_shader = bytes(rhi_test_compute_shader), .debug_name = name });
        REQUIRE(pipeline.has_value());
        return { *signature, *pipeline };
    }

    // A raw UAV buffer of `count` uint32 and the readback it is copied into
    struct fill_target
    {
        rhi::buffer_handle buffer;
        rhi::buffer_handle readback;
        std::uint32_t count;
    };

    fill_target make_fill_target(rhi::resources& resources, const std::uint32_t count = 64)
    {
        auto buffer = resources.create_buffer({ .size = count * 4u,
                                                .usage = buffer_usage::unordered_access | buffer_usage::copy_source,
                                                .debug_name = "fill target" });
        REQUIRE(buffer.has_value());
        return { *buffer, readback_buffer(resources, count * 4u), count };
    }

    // Dispatch the fill shader into `target`, then copy it to its readback buffer
    template<queue_type Queue>
    void record_fill(
        rhi::command_recorder<Queue> recorder,
        rhi::resources& resources,
        const fill_kit& fill,
        const fill_target& target,
        const std::uint32_t base)
    {
        recorder.set_pipeline(fill.pipeline);
        const std::array<std::uint32_t, 3> constants{ resources.uav(target.buffer).value, base, target.count };
        recorder.set_root_constants(0, constants);
        recorder.dispatch((target.count + 63) / 64, 1, 1);
        const std::array barriers{ rhi::buffer_barrier{ .buffer = target.buffer,
                                                        .before = { rhi::pipeline_synchronization::compute_shading,
                                                                    rhi::resource_access::unordered_access },
                                                        .after = { rhi::pipeline_synchronization::copy,
                                                                   rhi::resource_access::copy_source } } };
        recorder.barrier({}, barriers, {});
        recorder.copy_buffer(target.readback, 0, target.buffer, 0, target.count * 4u);
    }

    bool filled_with(rhi::resources& resources, const fill_target& target, const std::uint32_t base)
    {
        const std::span<std::byte> bytes = resources.mapped_bytes(target.readback);
        for (std::uint32_t index = 0; index < target.count; ++index)
        {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes.data() + index * 4, 4);
            if (value != base + index)
            {
                return false;
            }
        }
        return true;
    }

    // Upload a pattern into one subresource on the copy queue, read it back on the graphics queue
    bool texture_round_trip(
        tests::test_rhi& hardware_interface, rhi::texture_description description, const rhi::subresource subresource)
    {
        rhi::resources resources{ *hardware_interface };
        description.usage = texture_usage::copy_destination | texture_usage::copy_source | texture_usage::shader_resource;
        auto texture = resources.create_texture(description);
        REQUIRE(texture.has_value());
        const rhi::copy_footprint footprint = resources.copyable_footprint(*texture, subresource);
        REQUIRE(footprint.total_size > 0);

        const rhi::buffer_handle upload = upload_buffer(resources, footprint.total_size);
        const rhi::buffer_handle readback = readback_buffer(resources, footprint.total_size);
        const std::span<std::byte> source = resources.mapped_bytes(upload);
        for (std::uint32_t row = 0; row < footprint.row_count * footprint.depth; ++row)
        {
            for (std::uint64_t column = 0; column < footprint.row_size; ++column)
            {
                source[row * footprint.row_pitch + column] = static_cast<std::byte>((row * 7 + column * 13) & 0xFF);
            }
        }

        auto copy = acquire<queue_type::copy>(hardware_interface, "upload");
        copy.copy_buffer_to_texture(*texture, subresource, upload, 0);
        const rhi::synchronization_point uploaded = submit(hardware_interface, copy);

        auto graphics = acquire<queue_type::graphics>(hardware_interface, "readback");
        graphics.copy_texture_to_buffer(readback, 0, *texture, subresource);
        const std::array waits{ uploaded };
        hardware_interface.wait_for(submit(hardware_interface, graphics, waits));

        const std::span<std::byte> result = resources.mapped_bytes(readback);
        for (std::uint32_t row = 0; row < footprint.row_count * footprint.depth; ++row)
        {
            if (std::memcmp(result.data() + row * footprint.row_pitch, source.data() + row * footprint.row_pitch, footprint.row_size) != 0)
            {
                return false;
            }
        }
        return true;
    }
} // namespace

TEST_CASE("Upload and readback round trips across the copy and graphics queues", "[rhi]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };

    SECTION("a buffer")
    {
        constexpr std::uint64_t size = 4096;
        const rhi::buffer_handle upload = upload_buffer(resources, size);
        auto gpu = resources.create_buffer(
            { .size = size, .usage = buffer_usage::copy_destination | buffer_usage::copy_source, .debug_name = "gpu" });
        REQUIRE(gpu.has_value());
        const rhi::buffer_handle readback = readback_buffer(resources, size);
        const std::span<std::byte> source = resources.mapped_bytes(upload);
        for (std::size_t index = 0; index < size; ++index)
        {
            source[index] = static_cast<std::byte>(index * 31);
        }

        auto copy = acquire<queue_type::copy>(hardware_interface);
        copy.copy_buffer(*gpu, 0, upload, 0, size);
        const std::array waits{ submit(hardware_interface, copy) };
        auto graphics = acquire<queue_type::graphics>(hardware_interface);
        graphics.copy_buffer(readback, 0, *gpu, 0, size);
        hardware_interface.wait_for(submit(hardware_interface, graphics, waits));

        CHECK(std::memcmp(resources.mapped_bytes(readback).data(), source.data(), size) == 0);
    }

    SECTION("a texture")
    {
        CHECK(texture_round_trip(hardware_interface, { .width = 61, .height = 33, .format = format::rgba8_unorm }, {}));
    }

    SECTION("a BC texture")
    {
        CHECK(texture_round_trip(hardware_interface, { .width = 64, .height = 64, .format = format::bc1_unorm }, {}));
    }

    SECTION("a cube face and a mip")
    {
        CHECK(texture_round_trip(
            hardware_interface,
            { .dimension = rhi::texture_dimension::cube, .width = 16, .height = 16, .mip_levels = 2, .format = format::rgba8_unorm },
            { .mip = 1, .layer = 3 }));
    }
}

TEST_CASE("A compute dispatch writes through ResourceDescriptorHeap", "[rhi]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    const fill_kit fill = make_fill(hardware_interface);
    const fill_target target = make_fill_target(resources, 200);

    auto list = acquire<queue_type::compute>(hardware_interface, "fill");
    record_fill<queue_type::compute>(list, resources, fill, target, 1000);
    hardware_interface.wait_for(submit(hardware_interface, list));
    CHECK(filled_with(resources, target, 1000));
}

TEST_CASE("UAV clears on persistent and placed textures", "[rhi]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    rhi::memory memory{ *hardware_interface };
    rhi::transient_objects transient{ *hardware_interface };
    constexpr std::uint32_t size = 16;

    const auto read_uints = [&](const rhi::buffer_handle readback, const rhi::copy_footprint& footprint)
    {
        std::vector<std::uint32_t> values;
        const std::span<std::byte> bytes = resources.mapped_bytes(readback);
        for (std::uint32_t row = 0; row < footprint.row_count; ++row)
        {
            for (std::uint32_t column = 0; column < footprint.width; ++column)
            {
                std::uint32_t value = 0;
                std::memcpy(&value, bytes.data() + row * footprint.row_pitch + column * 4, 4);
                values.push_back(value);
            }
        }
        return values;
    };
    const rhi::access_state cleared{ rhi::pipeline_synchronization::clear_unordered_access_view,
                                     rhi::resource_access::unordered_access,
                                     rhi::texture_layout::unordered_access };
    const rhi::access_state copied{ rhi::pipeline_synchronization::copy,
                                    rhi::resource_access::copy_source,
                                    rhi::texture_layout::copy_source };

    SECTION("persistent: by mip")
    {
        auto texture = resources.create_texture({ .width = size,
                                                  .height = size,
                                                  .format = format::r32_uint,
                                                  .usage = texture_usage::unordered_access | texture_usage::copy_source,
                                                  .initial_layout = rhi::texture_layout::unordered_access });
        REQUIRE(texture.has_value());
        const rhi::copy_footprint footprint = resources.copyable_footprint(*texture, {});
        const rhi::buffer_handle readback = readback_buffer(resources, footprint.total_size);

        auto list = acquire<queue_type::graphics>(hardware_interface);
        const std::array<std::uint32_t, 4> value{ 7, 0, 0, 0 };
        list.clear_unordered_access_uint(*texture, 0, value);
        const std::array barriers{ rhi::texture_barrier{ .texture = *texture, .before = cleared, .after = copied } };
        list.barrier({}, {}, barriers);
        list.copy_texture_to_buffer(readback, 0, *texture, {});
        hardware_interface.wait_for(submit(hardware_interface, list));

        const std::vector<std::uint32_t> values = read_uints(readback, footprint);
        CHECK(std::ranges::all_of(values, [](const std::uint32_t entry) { return entry == 7; }));
    }

    SECTION("placed: by the range's UAV index (D-89)")
    {
        auto heap = memory.create_heap({ .size = 1 << 20 });
        REQUIRE(heap.has_value());
        auto texture = memory.create_placed_texture(
            *heap,
            0,
            { .width = size, .height = size, .format = format::r32_uint, .usage = texture_usage::unordered_access | texture_usage::copy_source });
        REQUIRE(texture.has_value());
        auto range = transient.create_descriptor_range(4);
        REQUIRE(range.has_value());
        const auto view = range->create_uav(*texture);
        REQUIRE(view.has_value());
        const rhi::copy_footprint footprint = resources.copyable_footprint(*texture, {});
        const rhi::buffer_handle readback = readback_buffer(resources, footprint.total_size);

        auto list = acquire<queue_type::graphics>(hardware_interface);
        const std::array first_use{ rhi::texture_barrier{ .texture = *texture, .before = rhi::access_states::undefined, .after = cleared } };
        list.barrier({}, {}, first_use);
        const std::array<std::uint32_t, 4> value{ 9, 0, 0, 0 };
        list.clear_unordered_access_uint(*texture, *view, value);
        const std::array barriers{ rhi::texture_barrier{ .texture = *texture, .before = cleared, .after = copied } };
        list.barrier({}, {}, barriers);
        list.copy_texture_to_buffer(readback, 0, *texture, {});
        hardware_interface.wait_for(submit(hardware_interface, list));

        const std::vector<std::uint32_t> values = read_uints(readback, footprint);
        CHECK(std::ranges::all_of(values, [](const std::uint32_t entry) { return entry == 9; }));
        resources.destroy(*texture);
        memory.destroy(*heap);
    }
}

TEST_CASE("A draw into a render target", "[rhi]")
{
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    rhi::pipelines pipelines{ *hardware_interface };
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;

    const std::array parameters{ rhi::root_parameter{ .kind = rhi::root_parameter_kind::constants,
                                                      .shader_register = 0,
                                                      .constant_count = 4,
                                                      .visibility = rhi::shader_visibility::pixel } };
    auto signature = pipelines.create_root_signature({ .parameters = parameters, .debug_name = "draw" });
    REQUIRE(signature.has_value());
    rhi::graphics_pipeline_description description{ .root_signature = *signature,
                                                    .vertex_shader = bytes(rhi_test_vertex_shader),
                                                    .pixel_shader = bytes(rhi_test_pixel_shader),
                                                    .debug_name = "triangle" };
    description.state.rasterizer.cull = rhi::cull_mode::none;
    description.state.depth.test = false;
    description.state.render_target_count = 1;
    description.state.render_target_formats[0] = format::rgba8_unorm;
    auto pipeline = pipelines.create_pipeline(description);
    REQUIRE(pipeline.has_value());

    auto target = resources.create_texture({ .width = width,
                                             .height = height,
                                             .format = format::rgba8_unorm,
                                             .usage = texture_usage::render_target | texture_usage::copy_source,
                                             .optimized_clear = rhi::clear_value{ .color = { 0.0f, 0.0f, 1.0f, 1.0f } },
                                             .initial_layout = rhi::texture_layout::render_target,
                                             .debug_name = "target" });
    REQUIRE(target.has_value());
    const rhi::copy_footprint footprint = resources.copyable_footprint(*target, {});
    const rhi::buffer_handle readback = readback_buffer(resources, footprint.total_size);

    auto list = acquire<queue_type::graphics>(hardware_interface, "draw");
    const rhi::texture_view view{ .texture = *target };
    const std::array<float, 4> blue{ 0.0f, 0.0f, 1.0f, 1.0f };
    list.clear_render_target(view, blue);
    list.set_render_targets(std::span{ &view, 1 });
    list.set_viewport({ .width = width / 2.0f, .height = static_cast<float>(height) }); // the left half
    list.set_scissor({ .right = width, .bottom = height });
    list.set_pipeline(*pipeline);
    list.set_root_constants(0, std::array{ 1.0f, 0.0f, 0.0f, 1.0f });
    list.draw(3);
    const std::array barriers{ rhi::texture_barrier{ .texture = *target,
                                                     .before = rhi::access_states::render_target,
                                                     .after = rhi::access_states::copy_source } };
    list.barrier({}, {}, barriers);
    list.copy_texture_to_buffer(readback, 0, *target, {});
    hardware_interface.wait_for(submit(hardware_interface, list));

    const std::span<std::byte> pixels = resources.mapped_bytes(readback);
    const auto pixel = [&](const std::uint32_t x, const std::uint32_t y)
    {
        std::array<std::uint8_t, 4> value{};
        std::memcpy(value.data(), pixels.data() + y * footprint.row_pitch + x * 4, 4);
        return value;
    };
    CHECK(pixel(5, 16) == std::array<std::uint8_t, 4>{ 255, 0, 0, 255 }); // drawn
    CHECK(pixel(60, 16) == std::array<std::uint8_t, 4>{ 0, 0, 255, 255 }); // cleared
}

TEST_CASE("Pipelines on two root signatures, recorded alternately", "[rhi]")
{
    tests::test_rhi hardware_interface{ with_gpu_validation() };
    rhi::resources resources{ *hardware_interface };
    const fill_kit first = make_fill(hardware_interface, 3, "fill on 3 constants");
    const fill_kit second = make_fill(hardware_interface, 8, "fill on 8 constants");
    const std::array targets{ make_fill_target(resources), make_fill_target(resources), make_fill_target(resources) };

    auto list = acquire<queue_type::compute>(hardware_interface);
    record_fill<queue_type::compute>(list, resources, first, targets[0], 10);
    record_fill<queue_type::compute>(list, resources, second, targets[1], 20);
    record_fill<queue_type::compute>(list, resources, first, targets[2], 30);
    hardware_interface.wait_for(submit(hardware_interface, list));

    CHECK(filled_with(resources, targets[0], 10));
    CHECK(filled_with(resources, targets[1], 20));
    CHECK(filled_with(resources, targets[2], 30));
}

TEST_CASE("A root call before set_pipeline asserts", "[rhi]")
{
    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    hardware_interface.accept_errors();

    auto list = acquire<queue_type::compute>(hardware_interface);
    list.set_root_constants(0, std::array<std::uint32_t, 1>{ 1 });
#ifndef NDEBUG
    CHECK(assertions.contains("no pipeline is bound yet"));
#endif
}

TEST_CASE("A compute recorder narrowed from a graphics list", "[rhi]")
{
    tests::recorded_assertions assertions;
    tests::test_rhi hardware_interface;
    rhi::resources resources{ *hardware_interface };
    rhi::transient_objects transient{ *hardware_interface };
    const fill_kit fill = make_fill(hardware_interface);
    const fill_target target = make_fill_target(resources);

    auto list = acquire<queue_type::graphics>(hardware_interface);
    const rhi::compute_recorder recorder = list.narrow<queue_type::compute>();
    CHECK(recorder.submission_queue() == queue_type::graphics);
    record_fill<queue_type::compute>(recorder, resources, fill, target, 5);

    SECTION("dispatches on the graphics queue")
    {
        hardware_interface.wait_for(submit(hardware_interface, list));
        CHECK(filled_with(resources, target, 5));
    }

    SECTION("takes timestamps of its list's queue")
    {
        hardware_interface.accept_errors();
        auto compute_set = transient.create_timestamp_query_set(queue_type::compute, 2);
        REQUIRE(compute_set.has_value());
        rhi::compute_recorder narrowed = recorder;
        narrowed.write_timestamp(*compute_set, 0);
#ifndef NDEBUG
        CHECK(assertions.contains("another queue"));
#endif
    }
}

TEST_CASE("Timestamps from two lists into one set", "[rhi]")
{
    tests::test_rhi hardware_interface{ with_gpu_validation() };
    rhi::transient_objects transient{ *hardware_interface };

    auto set = transient.create_timestamp_query_set(queue_type::graphics, 8, "timings");
    REQUIRE(set.has_value());
    auto first = acquire<queue_type::graphics>(hardware_interface, "A");
    auto second = acquire<queue_type::graphics>(hardware_interface, "B");
    first.write_timestamp(*set, 0);
    second.write_timestamp(*set, 2);
    first.write_timestamp(*set, 1);
    second.write_timestamp(*set, 3);
    first.write_timestamp(*set, 4);

    std::array lists{ std::move(first), std::move(second) };
    auto point = rhi::queue<queue_type::graphics>{ *hardware_interface }.submit(lists);
    REQUIRE(point.has_value());
    hardware_interface.wait_for(*point);

    const std::span<const std::uint64_t> ticks = set->ticks(0, 5);
    REQUIRE(ticks.size() == 5);
    CHECK(std::ranges::all_of(ticks, [](const std::uint64_t tick) { return tick != 0; }));
    CHECK(ticks[1] >= ticks[0]);
    CHECK(ticks[3] >= ticks[2]);

    SECTION("on the copy queue, when the adapter has copy-queue timestamps")
    {
        if (!hardware_interface->capabilities().copy_queue_timestamps)
        {
            SKIP("no copy-queue timestamps on this adapter");
        }
        auto copy_set = transient.create_timestamp_query_set(queue_type::copy, 2);
        REQUIRE(copy_set.has_value());
        auto copy = acquire<queue_type::copy>(hardware_interface);
        copy.write_timestamp(*copy_set, 0);
        copy.write_timestamp(*copy_set, 1);
        hardware_interface.wait_for(submit(hardware_interface, copy));
        CHECK(copy_set->ticks(0, 2)[0] != 0);
        CHECK(copy_set->ticks(0, 2)[1] >= copy_set->ticks(0, 2)[0]);
    }
}

TEST_CASE("Allocators are reused by synchronization point", "[rhi]")
{
    tests::test_rhi hardware_interface;
    const rhi::d3d12::command_list_pool& pool = hardware_interface->state().command_lists();

    for (int cycle = 0; cycle < 10; ++cycle)
    {
        auto list = acquire<queue_type::graphics>(hardware_interface);
        hardware_interface.wait_for(submit(hardware_interface, list));
    }
    const std::size_t warmed_up = pool.allocators_created();
    for (int cycle = 0; cycle < 1000; ++cycle)
    {
        auto list = acquire<queue_type::graphics>(hardware_interface);
        hardware_interface.wait_for(submit(hardware_interface, list));
    }
    CHECK(pool.allocators_created() == warmed_up);

    SECTION("a list destroyed without submit reuses its allocator at once")
    {
        {
            auto discarded = acquire<queue_type::compute>(hardware_interface);
        }
        const std::size_t before = pool.allocators_created();
        {
            auto again = acquire<queue_type::compute>(hardware_interface);
        }
        CHECK(pool.allocators_created() == before);
    }
}

TEST_CASE("Lists recorded and submitted from parallel workers", "[rhi]")
{
    constexpr std::size_t workers = 8;
    avis::runtime runtime{ tests::small_runtime_configuration(workers) };
    tests::test_rhi hardware_interface;
    rhi::command_lists lists{ *hardware_interface };
    rhi::queue<queue_type::graphics> graphics{ *hardware_interface };

    std::mutex submit_mutex;
    std::vector<rhi::synchronization_point> points;
    int failures = 0;
    auto work = stdexec::schedule(stdexec::get_parallel_scheduler()) |
                stdexec::bulk(stdexec::par,
                              workers * 4,
                              [&](std::size_t)
                              {
                                  auto list = lists.acquire<queue_type::graphics>("worker");
                                  const std::lock_guard lock{ submit_mutex };
                                  if (!list)
                                  {
                                      ++failures;
                                      return;
                                  }
                                  auto point = graphics.submit(std::span{ &*list, 1 });
                                  if (point)
                                  {
                                      points.push_back(*point);
                                  }
                                  else
                                  {
                                      ++failures;
                                  }
                              });
    REQUIRE(stdexec::sync_wait(std::move(work)).has_value());
    CHECK(failures == 0);
    REQUIRE(points.size() == workers * 4);
    hardware_interface.wait_for(graphics.last_submitted());
    for (const rhi::synchronization_point point : points)
    {
        CHECK(hardware_interface->state().queues().is_complete(point));
    }

    SECTION("an exited thread's slot number goes to the next new thread")
    {
        std::uint32_t first = 0;
        std::uint32_t second = 0;
        std::thread{ [&]
                     {
                         auto list = lists.acquire<queue_type::copy>();
                         first = rhi::d3d12::current_thread_slot();
                     } }
            .join();
        std::thread{ [&]
                     {
                         auto list = lists.acquire<queue_type::copy>();
                         second = rhi::d3d12::current_thread_slot();
                     } }
            .join();
        CHECK(first == second);
    }
}

TEST_CASE("A destroyed buffer waits for the open list that uses it", "[rhi]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    rhi::resources resources{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    const rhi::d3d12::deferred_release& releases = hardware_interface->state().lifetime().releases();

    auto source = resources.create_buffer({ .size = 256, .usage = buffer_usage::copy_source, .debug_name = "source" });
    auto destination = resources.create_buffer({ .size = 256, .usage = buffer_usage::copy_destination });
    REQUIRE(source.has_value());
    REQUIRE(destination.has_value());

    auto list = acquire<queue_type::graphics>(hardware_interface);
    list.copy_buffer(*destination, 0, *source, 0, 256);
    resources.destroy(*source);
    driver.poll_completions();
    CHECK(releases.parked() == 1); // the open list may still use it

    gate.point(queue_type::graphics, 1); // the list's submission waits behind the gate
    const rhi::synchronization_point point = submit(hardware_interface, list);
    driver.poll_completions();
    CHECK(releases.parked() == 0);
    CHECK(releases.tagged() == 1); // tagged with the list's submission, not complete yet

    gate.open(1);
    hardware_interface.wait_for(point);
    CHECK(releases.pending() == 0);
}

TEST_CASE("One barrier call with all three groups, including an aliasing discard", "[rhi]")
{
    tests::test_rhi hardware_interface{ with_gpu_validation() };
    rhi::resources resources{ *hardware_interface };
    rhi::memory memory{ *hardware_interface };

    auto heap = memory.create_heap({ .size = 1 << 20 });
    REQUIRE(heap.has_value());
    auto target = memory.create_placed_texture(
        *heap, 0, { .width = 64, .height = 64, .format = format::rgba8_unorm, .usage = texture_usage::render_target });
    REQUIRE(target.has_value());
    auto buffer = resources.create_buffer({ .size = 256, .usage = buffer_usage::unordered_access });
    REQUIRE(buffer.has_value());

    auto list = acquire<queue_type::graphics>(hardware_interface);
    const std::array globals{ rhi::global_barrier{ .before = { rhi::pipeline_synchronization::all, rhi::resource_access::common },
                                                   .after = { rhi::pipeline_synchronization::all, rhi::resource_access::common } } };
    const std::array buffers{ rhi::buffer_barrier{ .buffer = *buffer,
                                                   .before = rhi::access_states::common,
                                                   .after = rhi::access_states::unordered_access } };
    const std::array textures{ rhi::texture_barrier{ .texture = *target,
                                                     .before = rhi::access_states::undefined,
                                                     .after = rhi::access_states::render_target,
                                                     .discard = true } };
    list.barrier(globals, buffers, textures);
    const std::array<float, 4> black{ 0.0f, 0.0f, 0.0f, 1.0f };
    list.clear_render_target({ .texture = *target }, black);
    hardware_interface.wait_for(submit(hardware_interface, list));

    resources.destroy(*target);
    memory.destroy(*heap);
}
