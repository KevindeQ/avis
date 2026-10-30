// Lifetimes and aliasing placement of the render graph (docs/tasks/09-render-graph.md §9.6). The placement function on
// hand-built lifetimes and fixed sizes ([unit]), and lifetimes and checks on graphs over the test RHI, which execute
// nothing.

#include "helpers/recorded_assertions.h"
#include "helpers/render_graph_fixture.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/linear_arena.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/render_graph_compiler.h" // private, lib/avis/graphics/
#include "avis/graphics/texture.h"
#include "avis/rhi/barriers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

using namespace avis::graphics;
using avis::graphics::detail::compiled_graph;
using avis::graphics::detail::placement;
using avis::graphics::detail::placement_request;
using avis::tests::render_graph_fixture;

namespace
{
    constexpr std::uint64_t unit = std::uint64_t{ 64 } << 10; // 64 KiB, the placed-resource alignment

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

    placement_request request(const std::uint64_t units, const std::uint32_t first, const std::uint32_t last)
    {
        return { .size = units * unit, .alignment = unit, .first = first, .last = last };
    }

    bool lifetimes_overlap(const placement_request& a, const placement_request& b)
    {
        return a.first <= b.last && b.first <= a.last;
    }

    bool memory_overlaps(
        const placement& result,
        const std::span<const placement_request> requests,
        const std::size_t a,
        const std::size_t b)
    {
        return result.offsets[a] < result.offsets[b] + requests[b].size &&
               result.offsets[b] < result.offsets[a] + requests[a].size;
    }

    std::vector<std::uint64_t> offsets_of(const placement& result)
    {
        return { result.offsets.begin(), result.offsets.end() };
    }
} // namespace

TEST_CASE("placement: resources with overlapping lifetimes never share memory", "[render_graph][unit]")
{
    linear_arena arena;
    std::mt19937 random{ 20261030 };
    std::uniform_int_distribution<std::uint32_t> count_of(1, 40);
    std::uniform_int_distribution<std::uint64_t> size_of(1, 600'000);
    std::uniform_int_distribution<std::uint32_t> position_of(0, 60);
    std::uniform_int_distribution<int> alignment_of(0, 2);

    for (int graph = 0; graph < 1000; ++graph)
    {
        std::vector<placement_request> requests(count_of(random));
        for (placement_request& entry : requests)
        {
            const std::uint32_t a = position_of(random);
            const std::uint32_t b = position_of(random);
            entry = { .size = size_of(random),
                      .alignment = std::array<std::uint64_t, 3>{ 256, unit, 4 * unit }[alignment_of(random)],
                      .first = std::min(a, b),
                      .last = std::max(a, b) };
        }

        const placement result = detail::place(requests, true, arena);
        for (std::size_t a = 0; a < requests.size(); ++a)
        {
            CHECK(result.offsets[a] % requests[a].alignment == 0);
            CHECK(result.offsets[a] + requests[a].size <= result.statistics.heap_size);
            for (std::size_t b = a + 1; b < requests.size(); ++b)
            {
                if (lifetimes_overlap(requests[a], requests[b]) && memory_overlaps(result, requests, a, b))
                {
                    FAIL("graph " << graph << ": requests " << a << " and " << b << " overlap in time and memory");
                }
            }
        }
        arena.reset();
    }
}

TEST_CASE("placement: disjoint lifetimes share memory and smaller resources nest", "[render_graph][unit]")
{
    linear_arena arena;

    SECTION("two resources with disjoint lifetimes share offset 0")
    {
        const std::array requests{ request(2, 0, 1), request(2, 2, 3) };
        const placement result = detail::place(requests, true, arena);
        CHECK(offsets_of(result) == std::vector<std::uint64_t>{ 0, 0 });
        CHECK(result.statistics.heap_size == 2 * unit);
        CHECK(result.statistics.aliased_bytes == 4 * unit);
        CHECK(result.statistics.non_aliased_bytes == 0);
    }

    SECTION("three of decreasing size nest inside the largest")
    {
        // The middle and small ones overlap each other, not the large one: both fit inside its range
        const std::array requests{ request(4, 0, 0), request(2, 1, 2), request(1, 2, 2) };
        const placement result = detail::place(requests, true, arena);
        CHECK(offsets_of(result) == std::vector<std::uint64_t>{ 0, 0, 2 * unit });
        CHECK(result.statistics.heap_size == 4 * unit);
    }

    SECTION("a resource that isn't aliasable gets a range of its own")
    {
        std::array requests{ request(2, 0, 1), request(2, 2, 3) };
        requests[1].aliasable = false;
        const placement result = detail::place(requests, true, arena);
        CHECK(offsets_of(result) == std::vector<std::uint64_t>{ 0, 2 * unit });
        CHECK(result.statistics.aliased_bytes == 0);
        CHECK(result.statistics.non_aliased_bytes == 4 * unit);
    }
}

TEST_CASE("placement: a fixed set of resources gives a fixed placement", "[render_graph][unit]")
{
    linear_arena arena;
    const std::array requests{ request(4, 0, 3), request(2, 1, 2), request(2, 4, 6),
                               request(1, 2, 5), request(3, 5, 7), request(1, 0, 7) };
    // By size: 0 at 0; 4 at 0 (after 0's lifetime); 1 above 0; 2 above 4; 3 above all it meets; 5 above everything
    const std::vector<std::uint64_t> golden{ 0, 4 * unit, 3 * unit, 6 * unit, 0, 7 * unit };

    for (int run = 0; run < 3; ++run)
    {
        const placement result = detail::place(requests, true, arena);
        CHECK(offsets_of(result) == golden);
        CHECK(result.statistics.heap_size == 8 * unit);
        arena.reset();
    }
}

TEST_CASE("placement: with aliasing off the heap is the sum of the aligned sizes", "[render_graph][unit]")
{
    linear_arena arena;
    const std::array requests{ request(4, 0, 0),
                               request(2, 1, 1),
                               request(1, 2, 2),
                               placement_request{ .size = 1000, .alignment = unit, .first = 3, .last = 3 } };
    const placement result = detail::place(requests, false, arena);
    CHECK(result.statistics.heap_size == (4 + 2 + 1 + 1) * unit);
    CHECK(result.statistics.aliased_bytes == 0);
    CHECK(result.statistics.non_aliased_bytes == result.statistics.heap_size);
}

TEST_CASE(
    "render graph aliasing: a fixed graph places its transient resources the same way every frame", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    const auto build = [&]
    {
        graph.reset();
        const texture output = graph.import_texture(
            "output", avis::rhi::texture_handle{}, avis::rhi::access_states::common, avis::rhi::access_states::common);
        texture previous = output;
        for (int pass = 0; pass < 6; ++pass)
        {
            const texture input = previous;
            previous = graph
                           .add_graphics_pass<texture_data>(
                               "pass",
                               [&](render_pass_builder& builder, texture_data& data)
                               {
                                   if (pass > 0)
                                   {
                                       builder.read(input, texture_read::srv_pixel);
                                   }
                                   const std::uint32_t size = 16u << (pass % 3);
                                   data.output = builder.write(
                                       builder.create_texture("t", { .size = absolute(size, size) }),
                                       texture_write::render_target);
                               },
                               graphics_execute)
                           .output;
        }
        graph.add_graphics_pass<empty_data>(
            "present",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(previous, texture_read::srv_pixel);
                builder.write(output, texture_write::render_target);
            },
            graphics_execute);
        graph.compile();
        REQUIRE(graph.compiled() != nullptr);

        std::vector<std::uint64_t> offsets;
        for (const detail::compiled_resource& resource : graph.compiled()->resources)
        {
            offsets.push_back(resource.offset);
        }
        return offsets;
    };

    const std::vector<std::uint64_t> first = build();
    CHECK(first[0] == detail::no_offset); // the import isn't placed
    CHECK(graph.compiled()->statistics.heap_size > 0);
    for (int frame = 0; frame < 5; ++frame)
    {
        CHECK(build() == first);
    }
}

TEST_CASE("render graph aliasing: async compute lifetimes widen to the fences around them", "[render_graph]")
{
    // The first compute pass uses a scratch buffer only at position 1, and T lives at positions 2–3 on graphics.
    // Without async compute they may share memory; with it, the compute pass may run alongside T's passes until the
    // graphics batch that waits for the compute queue, so they must not.
    const bool async = GENERATE(true, false);
    render_graph_configuration configuration;
    configuration.async_compute = async;
    render_graph_fixture fixture{ configuration };
    render_graph& graph = fixture.graph;

    const buffer result = graph.import_buffer(
        "result", avis::rhi::buffer_handle{}, avis::rhi::access_states::common, avis::rhi::access_states::common);
    const texture output = graph.import_texture(
        "output", avis::rhi::texture_handle{}, avis::rhi::access_states::common, avis::rhi::access_states::common);

    const buffer_data& setup = graph.add_graphics_pass<buffer_data>(
        "setup",
        [&](render_pass_builder& builder, buffer_data& data)
        { data.output = builder.write(builder.create_buffer("input", { .size = 4096 }), buffer_write::uav); },
        graphics_execute);
    buffer scratch;
    const buffer_data& first_compute = graph.add_compute_pass<buffer_data>(
        "first compute",
        [&](render_pass_builder& builder, buffer_data& data)
        {
            builder.read(setup.output, buffer_read::srv_compute);
            scratch = builder.write(builder.create_buffer("scratch", { .size = 4096 }), buffer_write::uav);
            data.output = builder.write(builder.create_buffer("first result", { .size = 4096 }), buffer_write::uav);
        },
        compute_execute,
        compute_queue_hint::async_compute);
    const texture_data& draw = graph.add_graphics_pass<texture_data>(
        "draw",
        [&](render_pass_builder& builder, texture_data& data)
        {
            data.output =
                builder.write(builder.create_texture("t", { .size = absolute(16, 16) }), texture_write::render_target);
        },
        graphics_execute);
    graph.add_graphics_pass<empty_data>(
        "present",
        [&](render_pass_builder& builder, empty_data&)
        {
            builder.read(draw.output, texture_read::srv_pixel);
            builder.write(output, texture_write::render_target);
        },
        graphics_execute);
    const buffer_data& second_compute = graph.add_compute_pass<buffer_data>(
        "second compute",
        [&](render_pass_builder& builder, buffer_data& data)
        {
            builder.read(first_compute.output, buffer_read::srv_compute);
            data.output = builder.write(builder.create_buffer("second result", { .size = 4096 }), buffer_write::uav);
        },
        compute_execute,
        compute_queue_hint::async_compute);
    graph.add_graphics_pass<empty_data>(
        "consume",
        [&](render_pass_builder& builder, empty_data&)
        {
            builder.read(second_compute.output, buffer_read::srv_pixel);
            builder.write(result, buffer_write::uav);
        },
        graphics_execute);

    graph.compile();
    REQUIRE(graph.compiled() != nullptr);
    const compiled_graph& compiled = *graph.compiled();
    REQUIRE(compiled.order.size() == 6);
    const detail::compiled_resource& scratch_resource = compiled.resources[scratch.index()];
    const detail::compiled_resource& texture_resource = compiled.resources[draw.output.index()];
    REQUIRE(scratch_resource.requirements.size == texture_resource.requirements.size);
    CHECK(texture_resource.lifetime.first == 2);
    CHECK(texture_resource.lifetime.last == 3);

    const std::uint64_t size = scratch_resource.requirements.size;
    const bool shared = scratch_resource.offset < texture_resource.offset + size &&
                        texture_resource.offset < scratch_resource.offset + size;
    CHECK(scratch_resource.lifetime.first == 1);
    if (async)
    {
        CHECK(scratch_resource.lifetime.last == 4); // up to the batch that waits for the compute queue
        CHECK_FALSE(shared);
    }
    else
    {
        CHECK(scratch_resource.lifetime.last == 1); // free to share T's memory
    }
}

TEST_CASE("render graph aliasing: a partial first write of an aliased texture needs needs_clear", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;
    const avis::tests::recorded_assertions assertions;
    const bool flagged = GENERATE(false, true);

    graph.add_compute_pass<empty_data>(
        "partial",
        [&](render_pass_builder& builder, empty_data&)
        {
            const texture chain = builder.create_texture("chain", { .size = absolute(64, 64), .mip_levels = 4 });
            builder.write(
                chain,
                texture_write::uav,
                { .first_mip = 0, .mip_count = 1 },
                flagged ? write_flags::needs_clear : write_flags::none);
            builder.set_side_effects();
        },
        compute_execute);
    graph.compile();

#ifndef NDEBUG
    if (flagged)
    {
        CHECK(assertions.count() == 0);
    }
    else
    {
        CHECK(assertions.count() == 1);
        CHECK(assertions.contains("pass 'partial' writes only part of the fresh aliased texture 'chain'"));
    }
#endif
}
