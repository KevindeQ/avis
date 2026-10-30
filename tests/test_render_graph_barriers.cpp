// Barriers of compiled render graphs (docs/tasks/09-render-graph.md §9.5). On a graph over the test RHI, which executes
// nothing; the checks read the compiled_graph's barrier batches directly.

#include "helpers/render_graph_fixture.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/render_graph_compiler.h" // private, lib/avis/graphics/
#include "avis/graphics/texture.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/queue.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace avis::graphics;
using avis::graphics::detail::barrier_batch;
using avis::graphics::detail::buffer_transition;
using avis::graphics::detail::compiled_graph;
using avis::graphics::detail::texture_transition;
using avis::rhi::pipeline_synchronization;
using avis::rhi::queue_type;
using avis::rhi::resource_access;
using avis::rhi::texture_layout;
using avis::tests::render_graph_fixture;

namespace access_states = avis::rhi::access_states;

namespace
{
    struct texture_data
    {
        texture output;
    };

    struct buffer_data
    {
        buffer output;
    };

    struct empty_data
    {};

    constexpr auto graphics_execute = [](const auto&, const resource_bag&, graphics_pass_context&) {};
    constexpr auto compute_execute = [](const auto&, const resource_bag&, compute_pass_context&) {};
    constexpr auto copy_execute = [](const auto&, const resource_bag&, copy_pass_context&) {};

    const compiled_graph& compile(render_graph& graph)
    {
        graph.compile();
        REQUIRE(graph.compiled() != nullptr);
        return *graph.compiled();
    }

    std::vector<texture_transition> transitions_of(const barrier_batch& batch, const resource& handle)
    {
        std::vector<texture_transition> found;
        std::ranges::copy_if(
            batch.textures,
            std::back_inserter(found),
            [&](const texture_transition& transition) { return transition.resource == handle.index(); });
        return found;
    }

    std::vector<buffer_transition> buffer_transitions_of(const barrier_batch& batch, const resource& handle)
    {
        std::vector<buffer_transition> found;
        std::ranges::copy_if(
            batch.buffers,
            std::back_inserter(found),
            [&](const buffer_transition& transition) { return transition.resource == handle.index(); });
        return found;
    }

    texture import_output(render_graph& graph, const char* name)
    {
        return graph.import_texture(name, avis::rhi::texture_handle{}, access_states::common, access_states::common);
    }
} // namespace

TEST_CASE("render graph barriers: the minimal frame of 10 §3.6", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;
    graph.set_view_size({ 64, 32 });

    const texture output = import_output(graph, "output");
    const texture_data& triangle = graph.add_graphics_pass<texture_data>(
        "triangle",
        [&](render_pass_builder& builder, texture_data& data)
        {
            const texture color = builder.create_texture(
                "color", { .size = view_relative(1.0f), .format = avis::rhi::format::rgba8_unorm });
            data.output = builder.write(color, texture_write::render_target);
        },
        graphics_execute);
    graph.add_copy_pass<empty_data>(
        "present",
        [&](render_pass_builder& builder, empty_data&)
        {
            builder.read(triangle.output, texture_read::copy_source);
            builder.write(output, texture_write::copy_destination);
        },
        copy_execute);

    const compiled_graph& compiled = compile(graph);
    REQUIRE(compiled.order.size() == 2);
    CHECK(compiled.queues[1] == queue_type::graphics); // color isn't in common: the copy falls back to graphics

    // triangle: the transient color target's first use
    const std::vector<texture_transition> first = transitions_of(compiled.pass_barriers[0], triangle.output);
    REQUIRE(first.size() == 1);
    CHECK(first[0].before.layout == texture_layout::undefined);
    CHECK(first[0].discard);
    CHECK(first[0].after.layout == texture_layout::render_target);

    // present: render_target → copy_source on color, common → copy_destination on output
    const std::vector<texture_transition> color = transitions_of(compiled.pass_barriers[1], triangle.output);
    REQUIRE(color.size() == 1);
    CHECK(color[0].before.layout == texture_layout::render_target);
    CHECK(color[0].after.layout == texture_layout::copy_source);
    CHECK_FALSE(color[0].discard);
    const std::vector<texture_transition> present = transitions_of(compiled.pass_barriers[1], output);
    REQUIRE(present.size() == 1);
    CHECK(present[0].before.layout == texture_layout::common);
    CHECK(present[0].after.layout == texture_layout::copy_destination);

    // end of frame: copy_destination → common on output, in the graphics queue's trailing list
    const detail::end_of_frame_list& trailing = compiled.end_of_frame[static_cast<std::size_t>(queue_type::graphics)];
    const std::vector<texture_transition> final = transitions_of(trailing.barriers, output);
    REQUIRE(final.size() == 1);
    CHECK(final[0].before.layout == texture_layout::copy_destination);
    CHECK(final[0].after.layout == texture_layout::common);
    CHECK(transitions_of(trailing.barriers, triangle.output).empty());
    CHECK(compiled.end_of_frame[static_cast<std::size_t>(queue_type::compute)].barriers.empty());
    CHECK(compiled.end_of_frame[static_cast<std::size_t>(queue_type::copy)].barriers.empty());
}

TEST_CASE("render graph barriers: two passes reading one SRV share one barrier", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    const texture first_output = import_output(graph, "first");
    const buffer second_output =
        graph.import_buffer("second", avis::rhi::buffer_handle{}, access_states::common, access_states::common);
    const texture_data& producer = graph.add_graphics_pass<texture_data>(
        "producer",
        [&](render_pass_builder& builder, texture_data& data)
        {
            data.output = builder.write(
                builder.create_texture("lit", { .size = absolute(32, 32) }), texture_write::render_target);
        },
        graphics_execute);
    graph.add_graphics_pass<empty_data>(
        "pixel reader",
        [&](render_pass_builder& builder, empty_data&)
        {
            builder.read(producer.output, texture_read::srv_pixel);
            builder.write(first_output, texture_write::render_target);
        },
        graphics_execute);
    graph.add_compute_pass<empty_data>(
        "compute reader",
        [&](render_pass_builder& builder, empty_data&)
        {
            builder.read(producer.output, texture_read::srv_compute);
            builder.write(second_output, buffer_write::uav);
        },
        compute_execute);

    const compiled_graph& compiled = compile(graph);
    REQUIRE(compiled.order.size() == 3);

    const std::vector<texture_transition> before_first = transitions_of(compiled.pass_barriers[1], producer.output);
    REQUIRE(before_first.size() == 1);
    CHECK(before_first[0].before.layout == texture_layout::render_target);
    CHECK(before_first[0].after.layout == texture_layout::shader_resource);
    // The second read's synchronization is merged into the first barrier
    CHECK(
        before_first[0].after.synchronization ==
        (pipeline_synchronization::pixel_shading | pipeline_synchronization::compute_shading));
    CHECK(before_first[0].after.access == resource_access::shader_resource);
    CHECK(transitions_of(compiled.pass_barriers[2], producer.output).empty());
}

TEST_CASE("render graph barriers: UAV after UAV on one buffer is an access-only barrier", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    const buffer_data& first = graph.add_compute_pass<buffer_data>(
        "first",
        [&](render_pass_builder& builder, buffer_data& data)
        { data.output = builder.write(builder.create_buffer("counters", { .size = 4096 }), buffer_write::uav); },
        compute_execute);
    const buffer_data& second = graph.add_compute_pass<buffer_data>(
        "second",
        [&](render_pass_builder& builder, buffer_data& data)
        {
            data.output = builder.write(first.output, buffer_write::uav);
            builder.set_side_effects();
        },
        compute_execute);

    const compiled_graph& compiled = compile(graph);
    REQUIRE(compiled.order.size() == 2);

    // A fresh buffer that shares no memory needs no barrier
    CHECK(buffer_transitions_of(compiled.pass_barriers[0], first.output).empty());

    const std::vector<buffer_transition> between = buffer_transitions_of(compiled.pass_barriers[1], second.output);
    REQUIRE(between.size() == 1);
    CHECK(between[0].before.access == resource_access::unordered_access);
    CHECK(between[0].after.access == resource_access::unordered_access);
    CHECK(between[0].before.synchronization == pipeline_synchronization::compute_shading);
    CHECK(between[0].after.synchronization == pipeline_synchronization::compute_shading);
}

TEST_CASE("render graph barriers: a mip chain pass emits per-mip barriers", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    constexpr std::uint16_t mip_count = 4;
    const texture_data& linearise = graph.add_compute_pass<texture_data>(
        "linearise",
        [&](render_pass_builder& builder, texture_data& data)
        {
            const texture chain = builder.create_texture(
                "depth chain",
                { .size = absolute(64, 64), .mip_levels = mip_count, .format = avis::rhi::format::r32_float });
            data.output = builder.write(chain, texture_write::uav);
        },
        compute_execute);

    texture chain = linearise.output;
    for (std::uint16_t mip = 1; mip < mip_count; ++mip)
    {
        chain = graph
                    .add_compute_pass<texture_data>(
                        "downsample",
                        [&](render_pass_builder& builder, texture_data& data)
                        {
                            builder.read(
                                chain,
                                texture_read::srv_compute,
                                { .first_mip = static_cast<std::uint16_t>(mip - 1), .mip_count = 1 });
                            data.output =
                                builder.write(chain, texture_write::uav, { .first_mip = mip, .mip_count = 1 });
                            if (mip == mip_count - 1)
                            {
                                builder.set_side_effects();
                            }
                        },
                        compute_execute)
                    .output;
    }

    const compiled_graph& compiled = compile(graph);
    REQUIRE(compiled.order.size() == mip_count);

    // The first write covers the whole texture
    const std::vector<texture_transition> whole = transitions_of(compiled.pass_barriers[0], chain);
    REQUIRE(whole.size() == 1);
    CHECK(whole[0].range.mip_count == avis::rhi::subresource_range::all);

    for (std::uint32_t pass = 1; pass < mip_count; ++pass)
    {
        INFO("downsample pass " << pass);
        const std::vector<texture_transition> barriers = transitions_of(compiled.pass_barriers[pass], chain);
        REQUIRE(barriers.size() == 2);
        for (const texture_transition& transition : barriers)
        {
            CHECK(transition.range.mip_count == 1); // never a whole-resource barrier
            CHECK(transition.range.layer_count == 1);
        }
        const auto read =
            std::ranges::find(barriers, pass - 1, [](const texture_transition& t) { return t.range.first_mip; });
        const auto written =
            std::ranges::find(barriers, pass, [](const texture_transition& t) { return t.range.first_mip; });
        REQUIRE(read != barriers.end());
        REQUIRE(written != barriers.end());
        CHECK(read->before.layout == texture_layout::unordered_access);
        CHECK(read->after.layout == texture_layout::shader_resource);
        CHECK(written->before.layout == texture_layout::unordered_access); // UAV → UAV: access only
        CHECK(written->after.layout == texture_layout::unordered_access);
    }
}

TEST_CASE(
    "render graph barriers: a texture produced on async compute is handed over on the compute queue", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    const texture output = import_output(graph, "output");
    const texture_data& occlusion = graph.add_compute_pass<texture_data>(
        "occlusion",
        [&](render_pass_builder& builder, texture_data& data)
        {
            data.output = builder.write(
                builder.create_texture("ao", { .size = absolute(32, 32), .format = avis::rhi::format::r8_unorm }),
                texture_write::uav);
        },
        compute_execute,
        compute_queue_hint::async_compute);
    graph.add_graphics_pass<empty_data>(
        "lighting",
        [&](render_pass_builder& builder, empty_data&)
        {
            builder.read(occlusion.output, texture_read::srv_pixel);
            builder.write(output, texture_write::render_target);
        },
        graphics_execute);

    const compiled_graph& compiled = compile(graph);
    REQUIRE(compiled.batches.size() == 2);
    REQUIRE(compiled.batches[0].queue == queue_type::compute);
    CHECK(compiled.batches[0].signals);
    REQUIRE(compiled.batches[1].waits.size() == 1);

    // At the end of the compute batch, before its signal, into a layout both queues know
    const std::vector<texture_transition> handoff = transitions_of(compiled.batches[0].end_barriers, occlusion.output);
    REQUIRE(handoff.size() == 1);
    CHECK(handoff[0].before.layout == texture_layout::unordered_access);
    CHECK(handoff[0].after.layout == texture_layout::shader_resource);
    CHECK(handoff[0].after.access == resource_access::no_access);

    // The consumer starts from it: nothing left to transition on graphics
    CHECK(transitions_of(compiled.pass_barriers[1], occlusion.output).empty());
}

TEST_CASE("render graph barriers: a transient texture's first barrier is from undefined with discard", "[render_graph]")
{
    struct first_write
    {
        texture_write usage;
        texture_layout layout;
    };
    constexpr first_write writes[] = { { texture_write::render_target, texture_layout::render_target },
                                       { texture_write::depth_write, texture_layout::depth_write },
                                       { texture_write::uav, texture_layout::unordered_access },
                                       { texture_write::copy_destination, texture_layout::copy_destination } };

    for (const first_write& write : writes)
    {
        render_graph_fixture fixture;
        render_graph& graph = fixture.graph;
        const avis::rhi::format format =
            write.usage == texture_write::depth_write ? avis::rhi::format::d32_float : avis::rhi::format::rgba8_unorm;
        const texture_data& pass = graph.add_graphics_pass<texture_data>(
            "first use",
            [&](render_pass_builder& builder, texture_data& data)
            {
                data.output = builder.write(
                    builder.create_texture("fresh", { .size = absolute(16, 16), .format = format }), write.usage);
                builder.set_side_effects();
            },
            graphics_execute);

        const compiled_graph& compiled = compile(graph);
        const std::vector<texture_transition> first = transitions_of(compiled.pass_barriers[0], pass.output);
        REQUIRE(first.size() == 1);
        CHECK(first[0].before.layout == texture_layout::undefined);
        CHECK(first[0].before.access == resource_access::no_access);
        CHECK(first[0].discard);
        CHECK(first[0].after.layout == write.layout);
    }
}
