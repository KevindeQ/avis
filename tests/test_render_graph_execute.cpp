// Executing render graphs (docs/tasks/09-render-graph.md §9.8–9.9): the resource bag's contents and checks, recording
// in groups on the parallel scheduler, submission, and failures. On the test RHI, with frames of the fixture's
// scheduler and a small runtime for the parallel scheduler.

#include "helpers/graphics_fixture.h"
#include "helpers/recorded_assertions.h"
#include "helpers/render_graph_fixture.h"
#include "helpers/runtime_fixture.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/gpu_profiler.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/render_graph_executor.h" // private, lib/avis/graphics/
#include "avis/graphics/resource_bag.h"
#include "avis/graphics/texture.h"
#include "avis/graphics/upload_queue.h"
#include "avis/rhi/error.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/queue.h"
#include "avis/runtime.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <vector>

using namespace avis::graphics;
using avis::rhi::queue_type;
using avis::tests::render_graph_fixture;
namespace rhi = avis::rhi;

namespace
{
    struct texture_data
    {
        texture output;
    };

    struct empty_data
    {};

    constexpr auto graphics_execute = [](const auto&, const resource_bag&, graphics_pass_context&) {};

    frame_token begin_frame(render_graph_fixture& fixture)
    {
        std::optional<frame_token> frame = fixture.frames->try_begin_frame();
        REQUIRE(frame.has_value());
        return std::move(*frame);
    }

    // The last submission of each queue: unchanged when nothing was submitted
    using queue_points = std::array<rhi::synchronization_point, 3>;

    queue_points submitted_points(render_graph_fixture& fixture)
    {
        return { rhi::queue<queue_type::graphics>{ *fixture.hardware_interface }.last_submitted(),
                 rhi::queue<queue_type::compute>{ *fixture.hardware_interface }.last_submitted(),
                 rhi::queue<queue_type::copy>{ *fixture.hardware_interface }.last_submitted() };
    }

    texture import_output(render_graph& graph)
    {
        return graph.import_texture(
            "output", rhi::texture_handle{}, rhi::access_states::common, rhi::access_states::common);
    }

    // A producer writing a transient render target and a consumer reading it as an SRV and writing the import
    const texture_data& add_chain(render_graph& graph, const texture output, const texture_description& description)
    {
        const texture_data& producer = graph.add_graphics_pass<texture_data>(
            "producer",
            [&](render_pass_builder& builder, texture_data& data)
            {
                data.output = builder.write(builder.create_texture("color", description), texture_write::render_target);
            },
            graphics_execute);
        graph.add_graphics_pass<empty_data>(
            "consumer",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(producer.output, texture_read::srv_pixel);
                builder.write(output, texture_write::render_target);
            },
            graphics_execute);
        return producer;
    }
} // namespace

TEST_CASE("render graph materialise: the bag maps transient resources to placed ones", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;
    graph.set_view_size({ 200, 100 });

    const texture output = import_output(graph);
    const texture_data& producer =
        add_chain(graph, output, { .size = view_relative(0.5f), .format = rhi::format::rgba16_float });
    graph.compile();

    frame_token frame = begin_frame(fixture);
    const auto bag = graph.materialise(frame);
    REQUIRE(bag.has_value());
    const resource_bag& resources = **bag;

    CHECK(resources.width(producer.output) == 100);
    CHECK(resources.height(producer.output) == 50);
    const texture_description& description = resources.description(producer.output);
    CHECK(description.size == absolute(100, 50));
    CHECK(description.format == rhi::format::rgba16_float);

    const rhi::texture_handle color = resources.texture_handle(producer.output);
    REQUIRE(color.is_valid());
    const rhi::texture_description& placed = rhi::resources{ *fixture.hardware_interface }.description(color);
    CHECK(placed.width == 100);
    CHECK(placed.height == 50);
    CHECK(static_cast<bool>(placed.usage & rhi::texture_usage::render_target));
    CHECK(static_cast<bool>(placed.usage & rhi::texture_usage::shader_resource));
    CHECK(resources.srv(producer.output).value != 0); // read as an SRV: a view in the frame's range
    CHECK(resources.render_target_view(producer.output).texture == color);
    CHECK_FALSE(resources.texture_handle(output).is_valid()); // the import's own (here invalid) handle

    fixture.frames->end_frame(std::move(frame), {});
}

TEST_CASE("render graph materialise: a pass looking up a resource it didn't declare fails a check", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    const texture output = import_output(graph);
    const texture_data& producer = add_chain(graph, output, { .size = absolute(16, 16) });
    graph.compile();

    frame_token frame = begin_frame(fixture);
    const auto bag = graph.materialise(frame);
    REQUIRE(bag.has_value());
    const resource_bag& resources = **bag;
    const avis::tests::recorded_assertions assertions;

    {
        // The consumer declared the color texture: no check
        const detail::current_pass_scope scope{ graph.tables().passes[1] };
        CHECK(resources.texture_handle(producer.output).is_valid());
        CHECK(assertions.count() == 0);
    }
    {
        // The producer didn't declare the import
        const detail::current_pass_scope scope{ graph.tables().passes[0] };
        resources.texture_handle(output);
#ifndef NDEBUG
        CHECK(assertions.count() == 1);
        CHECK(assertions.contains("pass 'producer' looked up resource"));
#endif
    }

    fixture.frames->end_frame(std::move(frame), {});
}

TEST_CASE("render graph execute: running out of frame descriptors fails before recording", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture{ {}, 16 };
    render_graph& graph = fixture.graph;
    const queue_points before = submitted_points(fixture);

    // 20 transient textures, each read as an SRV: 20 views in a range of 16
    const texture output = import_output(graph);
    std::vector<texture> textures;
    for (int index = 0; index < 20; ++index)
    {
        textures.push_back(graph
                               .add_graphics_pass<texture_data>(
                                   "producer",
                                   [&](render_pass_builder& builder, texture_data& data)
                                   {
                                       data.output = builder.write(
                                           builder.create_texture("color", { .size = absolute(8, 8) }),
                                           texture_write::render_target);
                                   },
                                   graphics_execute)
                               .output);
    }
    graph.add_graphics_pass<empty_data>(
        "consumer",
        [&](render_pass_builder& builder, empty_data&)
        {
            for (const texture input : textures)
            {
                builder.read(input, texture_read::srv_pixel);
            }
            builder.write(output, texture_write::render_target);
        },
        graphics_execute);
    graph.compile();

    frame_token frame = fixture.begin_frame();
    const auto submitted = fixture.execute(frame);
    REQUIRE_FALSE(submitted.has_value());
    CHECK(rhi::is_error(submitted.error(), rhi::error_kind::transient_memory_exhausted));
    CHECK(submitted_points(fixture) == before); // nothing submitted
    fixture.frames->end_frame(std::move(frame), {});

    // The next frame works
    graph.reset();
    const texture next_output = import_output(graph);
    add_chain(graph, next_output, { .size = absolute(8, 8) });
    graph.compile();
    frame_token next = fixture.begin_frame();
    const auto next_submitted = fixture.execute(next);
    REQUIRE(next_submitted.has_value());
    fixture.wait(*next_submitted);
    fixture.frames->end_frame(std::move(next), next_submitted->last_submissions());
}

TEST_CASE("render graph execute: passes record on parallel workers, in groups", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    constexpr int pass_count = 20;
    std::array<std::atomic<int>, pass_count> runs{};
    std::atomic<int> on_workers{ 0 };
    for (int index = 0; index < pass_count; ++index)
    {
        graph.add_graphics_pass<empty_data>(
            "pass",
            [](render_pass_builder& builder, empty_data&) { builder.set_side_effects(); },
            [&, index](const empty_data&, const resource_bag&, graphics_pass_context&)
            {
                runs[index].fetch_add(1);
                if (avis::threading::detail::in_parallel_work())
                {
                    on_workers.fetch_add(1);
                }
            });
    }
    graph.compile();

    frame_token frame = fixture.begin_frame();
    const auto submitted = fixture.execute(frame);
    REQUIRE(submitted.has_value());
    CHECK(graph.debug_information().recording_groups == 5); // 4 passes per group without timings
    CHECK(std::ranges::all_of(runs, [](const std::atomic<int>& count) { return count.load() == 1; }));
    CHECK(on_workers.load() == pass_count);
    REQUIRE(submitted->count == 1);
    CHECK(submitted->points[0].queue == queue_type::graphics);
    fixture.wait(*submitted);
    fixture.frames->end_frame(std::move(frame), submitted->last_submissions());
}

TEST_CASE("render graph execute: a compute pass on the graphics queue records into the graphics list", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    std::optional<queue_type> seen;
    graph.add_compute_pass<empty_data>(
        "compute",
        [](render_pass_builder& builder, empty_data&) { builder.set_side_effects(); },
        [&](const empty_data&, const resource_bag&, compute_pass_context& context)
        { seen = context.recorder().submission_queue(); });
    graph.compile();

    frame_token frame = fixture.begin_frame();
    const auto submitted = fixture.execute(frame);
    REQUIRE(submitted.has_value());
    CHECK(seen == queue_type::graphics);
    fixture.wait(*submitted);
    fixture.frames->end_frame(std::move(frame), submitted->last_submissions());
}

TEST_CASE("render graph execute: an import's ready_after point is waited on", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;
    rhi::resources resources{ *fixture.hardware_interface };
    const auto uploads = avis::tests::make_test_upload_queue(fixture.hardware_interface);

    constexpr std::uint64_t size = 1024;
    std::vector<std::uint32_t> pattern(size / 4);
    for (std::size_t index = 0; index < pattern.size(); ++index)
    {
        pattern[index] = static_cast<std::uint32_t>(index * 7 + 3);
    }
    const auto source = resources.create_buffer(
        { .size = size, .usage = rhi::buffer_usage::copy_destination | rhi::buffer_usage::copy_source });
    const auto readback = resources.create_buffer(
        { .size = size, .usage = rhi::buffer_usage::copy_destination, .memory = rhi::memory_type::readback });
    REQUIRE(source.has_value());
    REQUIRE(readback.has_value());

    const bool complete = GENERATE(false, true);
    REQUIRE(uploads->upload_buffer(*source, 0, std::as_bytes(std::span{ pattern })).has_value());
    const auto flushed = uploads->flush();
    REQUIRE(flushed.has_value());
    if (complete)
    {
        fixture.hardware_interface.wait_for(*flushed); // already complete: the RHI drops the wait
    }

    struct copy_data
    {
        buffer source;
        buffer destination;
    };
    const buffer uploaded =
        graph.import_buffer("uploaded", *source, rhi::access_states::common, rhi::access_states::common, *flushed);
    const buffer output =
        graph.import_buffer("output", *readback, rhi::access_states::common, rhi::access_states::common);
    graph.add_graphics_pass<copy_data>(
        "copy",
        [&](render_pass_builder& builder, copy_data& data)
        {
            data.source = builder.read(uploaded, buffer_read::copy_source);
            data.destination = builder.write(output, buffer_write::copy_destination);
        },
        [](const copy_data& data, const resource_bag& bag, graphics_pass_context& context)
        {
            context.recorder().copy_buffer(
                bag.buffer_handle(data.destination), 0, bag.buffer_handle(data.source), 0, size);
        });
    graph.compile();
    REQUIRE(graph.compiled()->batches.size() == 1);
    CHECK(graph.compiled()->batches[0].ready_after.size() == 1);

    frame_token frame = fixture.begin_frame();
    const auto submitted = fixture.execute(frame);
    REQUIRE(submitted.has_value());
    fixture.wait(*submitted);
    std::vector<std::uint32_t> read(pattern.size());
    std::memcpy(read.data(), resources.mapped_bytes(*readback).data(), size);
    CHECK(read == pattern);
    fixture.frames->end_frame(std::move(frame), submitted->last_submissions());

    resources.destroy(*source);
    resources.destroy(*readback);
}

TEST_CASE("render graph execute: submitted_frame covers every queue used", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    // An async compute pass nobody waits on, and a graphics pass
    graph.add_compute_pass<empty_data>(
        "histogram",
        [](render_pass_builder& builder, empty_data&) { builder.set_side_effects(); },
        [](const empty_data&, const resource_bag&, compute_pass_context&) {},
        compute_queue_hint::async_compute);
    graph.add_graphics_pass<empty_data>(
        "draw", [](render_pass_builder& builder, empty_data&) { builder.set_side_effects(); }, graphics_execute);
    graph.compile();
    REQUIRE(graph.compiled()->batches.size() == 2);
    CHECK(graph.compiled()->batches[1].waits.empty());

    frame_token frame = fixture.begin_frame();
    const auto submitted = fixture.execute(frame);
    REQUIRE(submitted.has_value());
    REQUIRE(submitted->count == 2);
    CHECK(submitted->points[0].queue == queue_type::graphics);
    CHECK(submitted->points[1].queue == queue_type::compute);
    CHECK(submitted->points[1] == rhi::queue<queue_type::compute>{ *fixture.hardware_interface }.last_submitted());
    fixture.wait(*submitted);
    fixture.frames->end_frame(std::move(frame), submitted->last_submissions());
}

TEST_CASE("render graph execute: an execute callback that runs out of frame memory fails the frame", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;
    const queue_points before = submitted_points(fixture);

    const auto build = [&](const bool exhaust)
    {
        graph.reset();
        for (int index = 0; index < 8; ++index)
        {
            graph.add_compute_pass<empty_data>(
                "pass",
                [](render_pass_builder& builder, empty_data&) { builder.set_side_effects(); },
                [exhaust, index](const empty_data&, const resource_bag&, compute_pass_context& context)
                {
                    if (exhaust && index == 5)
                    {
                        // What allocate_constants throws when the frame's memory is full
                        throw avis::runtime_error{ rhi::make_error(
                            rhi::error_kind::transient_memory_exhausted, "frame memory exhausted") };
                    }
                    context.allocate_constants(std::uint32_t{ 1 });
                },
                index % 2 == 0 ? compute_queue_hint::async_compute : compute_queue_hint::graphics);
        }
        graph.compile();
    };

    build(true);
    frame_token frame = fixture.begin_frame();
    const auto failed = fixture.execute(frame);
    REQUIRE_FALSE(failed.has_value());
    CHECK(rhi::is_error(failed.error(), rhi::error_kind::transient_memory_exhausted));
    CHECK(submitted_points(fixture) == before);
    fixture.frames->end_frame(std::move(frame), {});

    build(false);
    frame_token next = fixture.begin_frame();
    const auto submitted = fixture.execute(next);
    REQUIRE(submitted.has_value());
    CHECK(submitted->count == 2);
    fixture.wait(*submitted);
    fixture.frames->end_frame(std::move(next), submitted->last_submissions());
}

TEST_CASE("render graph execute: a stop request completes with set_stopped and submits nothing", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;
    const queue_points before = submitted_points(fixture);
    graph.add_graphics_pass<empty_data>(
        "draw", [](render_pass_builder& builder, empty_data&) { builder.set_side_effects(); }, graphics_execute);
    graph.compile();

    frame_token frame = fixture.begin_frame();
    stdexec::inplace_stop_source stop;
    stop.request_stop();
    const auto outcome = avis::threading::sync_wait_result(
        stdexec::write_env(graph.execute(frame), stdexec::prop{ stdexec::get_stop_token, stop.get_token() }));
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().domain == avis::error_domain::generic);
    CHECK(outcome.error().code == static_cast<std::int32_t>(avis::generic_error_kind::cancelled));
    CHECK(submitted_points(fixture) == before);
    fixture.frames->end_frame(std::move(frame), {});
}

TEST_CASE("render graph execute: with a profiler, passes are timed and groups sized from the timings", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture{ avis::tests::render_graph_fixture_options{ .profiler = true } };
    render_graph& graph = fixture.graph;
    constexpr std::uint32_t frames_in_flight = 2;

    std::vector<std::uint32_t> groups;
    for (std::uint32_t frame_number = 0; frame_number < frames_in_flight + 2; ++frame_number)
    {
        graph.reset();
        frame_token frame = fixture.begin_frame();
        for (int index = 0; index < 12; ++index)
        {
            graph.add_graphics_pass<empty_data>(
                std::format("pass {}", index),
                [](render_pass_builder& builder, empty_data&) { builder.set_side_effects(); },
                graphics_execute);
        }
        graph.compile();

        if (frame_number == frames_in_flight)
        {
            // Frame 1's timings: one per pass, named after it
            const std::span<const gpu_timing> timings = fixture.profiler->completed_gpu_timings(frame);
            for (int index = 0; index < 12; ++index)
            {
                INFO("pass " << index);
                CHECK(std::ranges::count(timings, std::format("pass {}", index), &gpu_timing::name) == 1);
            }
        }

        const auto submitted = fixture.execute(frame);
        REQUIRE(submitted.has_value());
        groups.push_back(graph.debug_information().recording_groups);
        if (frame_number >= frames_in_flight)
        {
            CHECK(graph.debug_information().passes[0].gpu_milliseconds.has_value());
        }
        fixture.wait(*submitted);
        fixture.frames->end_frame(std::move(frame), submitted->last_submissions());
    }

    // 12 passes of 4 without timings; then sized from them: empty passes take far less than 1 ms, so one group
    CHECK(groups == std::vector<std::uint32_t>{ 3, 3, 1, 1 });
}
