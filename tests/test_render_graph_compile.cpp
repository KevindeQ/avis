// Compiling render graphs (docs/tasks/09-render-graph.md §9.4, §9.10): culling, order, queue assignment, batches and
// the debug information. On a graph over the test RHI, which executes nothing; the checks read the compiled_graph
// directly.

#include "helpers/render_graph_fixture.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/graph_debug_information.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/render_graph_compiler.h" // private, lib/avis/graphics/
#include "avis/graphics/texture.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/queue.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <vector>

using namespace avis::graphics;
using avis::graphics::detail::compiled_graph;
using avis::graphics::detail::submission_batch;
using avis::rhi::queue_type;
using avis::tests::render_graph_fixture;

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

    texture import_output(render_graph& graph, const char* name = "output")
    {
        return graph.import_texture(
            name, avis::rhi::texture_handle{}, avis::rhi::access_states::common, avis::rhi::access_states::common);
    }

    buffer import_buffer(
        render_graph& graph,
        const char* name,
        const std::optional<avis::rhi::synchronization_point> ready_after = std::nullopt)
    {
        return graph.import_buffer(
            name,
            avis::rhi::buffer_handle{},
            avis::rhi::access_states::common,
            avis::rhi::access_states::common,
            ready_after);
    }

    // A graphics pass writing a new transient texture, reading `inputs` as SRVs
    const texture_data& add_producer(render_graph& graph, const char* name, std::span<const texture> inputs = {})
    {
        return graph.add_graphics_pass<texture_data>(
            name,
            [&](render_pass_builder& builder, texture_data& data)
            {
                for (const texture input : inputs)
                {
                    builder.read(input, texture_read::srv_pixel);
                }
                data.output = builder.write(
                    builder.create_texture(name, { .size = absolute(16, 16) }), texture_write::render_target);
            },
            graphics_execute);
    }

    // A graphics pass reading `input` and writing the import `output`
    void add_present(render_graph& graph, const char* name, const texture input, const texture output)
    {
        graph.add_graphics_pass<empty_data>(
            name,
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(input, texture_read::srv_pixel);
                builder.write(output, texture_write::render_target);
            },
            graphics_execute);
    }

    const compiled_graph& compile(render_graph& graph)
    {
        graph.compile();
        REQUIRE(graph.compiled() != nullptr);
        return *graph.compiled();
    }

    std::vector<std::uint32_t> order_of(const compiled_graph& compiled)
    {
        return { compiled.order.begin(), compiled.order.end() };
    }

    std::vector<std::uint32_t> waits_of(const submission_batch& batch)
    {
        return { batch.waits.begin(), batch.waits.end() };
    }
} // namespace

TEST_CASE("render graph culling", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    SECTION("a chain whose end nobody reads is culled entirely")
    {
        const texture a = add_producer(graph, "a").output;
        const texture b = add_producer(graph, "b", std::span{ &a, 1 }).output;
        add_producer(graph, "c", std::span{ &b, 1 });

        const compiled_graph& compiled = compile(graph);
        CHECK(compiled.order.empty());
        CHECK(compiled.batches.empty());
        CHECK(std::ranges::all_of(compiled.culled, [](const std::uint8_t culled) { return culled == 1; }));
    }

    SECTION("a pass with side effects keeps its producers")
    {
        const texture a = add_producer(graph, "a").output;
        add_producer(graph, "unused");
        graph.add_graphics_pass<empty_data>(
            "debug",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(a, texture_read::srv_pixel);
                builder.set_side_effects();
            },
            graphics_execute);

        const compiled_graph& compiled = compile(graph);
        CHECK(order_of(compiled) == std::vector<std::uint32_t>{ 0, 2 });
        CHECK(compiled.culled[1] == 1);
    }

    SECTION("a pass writing an import is kept, with its producers")
    {
        const texture output = import_output(graph);
        const texture a = add_producer(graph, "a").output;
        add_present(graph, "present", a, output);

        const compiled_graph& compiled = compile(graph);
        CHECK(order_of(compiled) == std::vector<std::uint32_t>{ 0, 1 });
    }

    SECTION("of two branches, only the one feeding the import is kept")
    {
        const texture output = import_output(graph);
        const texture kept_first = add_producer(graph, "kept first").output;
        const texture dropped_first = add_producer(graph, "dropped first").output;
        const texture kept_second = add_producer(graph, "kept second", std::span{ &kept_first, 1 }).output;
        add_producer(graph, "dropped second", std::span{ &dropped_first, 1 });
        add_present(graph, "present", kept_second, output);

        const compiled_graph& compiled = compile(graph);
        CHECK(order_of(compiled) == std::vector<std::uint32_t>{ 0, 2, 4 });
        CHECK(compiled.culled[1] == 1);
        CHECK(compiled.culled[3] == 1);
    }

    SECTION("drawing more into a texture keeps the pass that drew first")
    {
        const texture output = import_output(graph);
        const texture first = add_producer(graph, "first").output;
        const texture_data& second = graph.add_graphics_pass<texture_data>(
            "second",
            [&](render_pass_builder& builder, texture_data& data)
            { data.output = builder.write(first, texture_write::render_target); },
            graphics_execute);
        add_present(graph, "present", second.output, output);

        CHECK(order_of(compile(graph)) == std::vector<std::uint32_t>{ 0, 1, 2 });
    }

    SECTION("a pass reading its own output alone isn't kept by it")
    {
        graph.add_compute_pass<empty_data>(
            "self",
            [&](render_pass_builder& builder, empty_data&)
            {
                const buffer scratch =
                    builder.write(builder.create_buffer("scratch", { .size = 256 }), buffer_write::uav);
                builder.read(scratch, buffer_read::srv_compute);
            },
            compute_execute);

        CHECK(compile(graph).order.empty());
    }
}

TEST_CASE("render graph order is declaration order minus culled passes", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    const texture output = import_output(graph);
    std::vector<texture> kept;
    for (int index = 0; index < 10; ++index)
    {
        const std::string name = std::format("pass {}", index);
        const texture produced = add_producer(graph, name.c_str()).output;
        if (index % 3 != 1)
        {
            kept.push_back(produced);
        }
    }
    graph.add_graphics_pass<empty_data>(
        "present",
        [&](render_pass_builder& builder, empty_data&)
        {
            for (const texture input : kept)
            {
                builder.read(input, texture_read::srv_pixel);
            }
            builder.write(output, texture_write::render_target);
        },
        graphics_execute);

    CHECK(order_of(compile(graph)) == std::vector<std::uint32_t>{ 0, 2, 3, 5, 6, 8, 9, 10 });
}

TEST_CASE("render graph queue assignment", "[render_graph]")
{
    SECTION("an async-hinted compute pass goes to the compute queue, or to graphics when async compute is off")
    {
        const bool async_compute = GENERATE(true, false);
        render_graph_fixture fixture{ { .async_compute = async_compute } };
        render_graph& graph = fixture.graph;

        const buffer output = import_buffer(graph, "output");
        graph.add_compute_pass<empty_data>(
            "async",
            [&](render_pass_builder& builder, empty_data&) { builder.write(output, buffer_write::uav); },
            compute_execute,
            compute_queue_hint::async_compute);
        graph.add_compute_pass<empty_data>(
            "inline", [&](render_pass_builder& builder, empty_data&) { builder.set_side_effects(); }, compute_execute);

        const compiled_graph& compiled = compile(graph);
        CHECK(compiled.queues[0] == (async_compute ? queue_type::compute : queue_type::graphics));
        CHECK(compiled.queues[1] == queue_type::graphics);
    }

    SECTION("a copy of two buffers goes to the copy queue")
    {
        render_graph_fixture fixture;
        render_graph& graph = fixture.graph;

        const buffer source = import_buffer(graph, "source");
        const buffer destination = import_buffer(graph, "destination");
        graph.add_copy_pass<empty_data>(
            "copy",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(source, buffer_read::copy_source);
                builder.write(destination, buffer_write::copy_destination);
            },
            copy_execute);

        CHECK(compile(graph).queues[0] == queue_type::copy);
    }

    SECTION("a copy of a render target goes to graphics (the §3.6 frame)")
    {
        render_graph_fixture fixture;
        render_graph& graph = fixture.graph;

        const texture output = import_output(graph);
        const texture color = add_producer(graph, "triangle").output;
        graph.add_copy_pass<empty_data>(
            "present",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(color, texture_read::copy_source);
                builder.write(output, texture_write::copy_destination);
            },
            copy_execute);

        const compiled_graph& compiled = compile(graph);
        CHECK(order_of(compiled) == std::vector<std::uint32_t>{ 0, 1 });
        CHECK(compiled.queues[1] == queue_type::graphics);
        // Both passes form one batch and one submission
        REQUIRE(compiled.batches.size() == 1);
        CHECK(compiled.batches[0].passes.size() == 2);
    }

    SECTION("a copy of textures in the common layout goes to the copy queue, and leaves them there")
    {
        render_graph_fixture fixture;
        render_graph& graph = fixture.graph;

        const texture source = import_output(graph, "source");
        const texture destination = import_output(graph, "destination");
        const texture final_destination = import_output(graph, "final");
        const texture_data& first = graph.add_copy_pass<texture_data>(
            "first copy",
            [&](render_pass_builder& builder, texture_data& data)
            {
                builder.read(source, texture_read::copy_source);
                data.output = builder.write(destination, texture_write::copy_destination);
            },
            copy_execute);
        graph.add_copy_pass<empty_data>(
            "second copy",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(first.output, texture_read::copy_source);
                builder.write(final_destination, texture_write::copy_destination);
            },
            copy_execute);

        const compiled_graph& compiled = compile(graph);
        CHECK(compiled.queues[0] == queue_type::copy);
        CHECK(compiled.queues[1] == queue_type::copy);
    }
}

TEST_CASE("render graph batches and cross-queue waits", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    const buffer output = import_buffer(graph, "output");
    const texture source = add_producer(graph, "graphics source").output; // pass 0
    const buffer_data& async = graph.add_compute_pass<buffer_data>( // pass 1
        "async",
        [&](render_pass_builder& builder, buffer_data& data)
        {
            builder.read(source, texture_read::srv_compute);
            data.output = builder.write(builder.create_buffer("histogram", { .size = 1024 }), buffer_write::uav);
        },
        compute_execute,
        compute_queue_hint::async_compute);

    SECTION("graphics → async compute → graphics: three batches, one signal and one wait each way")
    {
        graph.add_compute_pass<empty_data>( // pass 2
            "consumer",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(async.output, buffer_read::srv_compute);
                builder.write(output, buffer_write::uav);
            },
            compute_execute);

        const compiled_graph& compiled = compile(graph);
        REQUIRE(compiled.batches.size() == 3);
        CHECK(compiled.batches[0].queue == queue_type::graphics);
        CHECK(compiled.batches[1].queue == queue_type::compute);
        CHECK(compiled.batches[2].queue == queue_type::graphics);

        CHECK(compiled.batches[0].waits.empty());
        CHECK(compiled.batches[0].signals);
        CHECK(waits_of(compiled.batches[1]) == std::vector<std::uint32_t>{ 0 });
        CHECK(compiled.batches[1].signals);
        CHECK(waits_of(compiled.batches[2]) == std::vector<std::uint32_t>{ 1 });
        CHECK_FALSE(compiled.batches[2].signals);
        CHECK(compiled.pass_batches[2] == 2);
    }

    SECTION("a second consumer on graphics doesn't wait again")
    {
        graph.add_compute_pass<empty_data>( // pass 2
            "first consumer",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(async.output, buffer_read::srv_compute);
                builder.set_side_effects();
            },
            compute_execute);
        graph.add_compute_pass<empty_data>( // pass 3, on compute: splits the graphics passes into two batches
            "other async work",
            [&](render_pass_builder& builder, empty_data&) { builder.set_side_effects(); },
            compute_execute,
            compute_queue_hint::async_compute);
        graph.add_compute_pass<empty_data>( // pass 4
            "second consumer",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(async.output, buffer_read::srv_compute);
                builder.write(output, buffer_write::uav);
            },
            compute_execute);

        const compiled_graph& compiled = compile(graph);
        REQUIRE(compiled.batches.size() == 5);
        CHECK(waits_of(compiled.batches[2]) == std::vector<std::uint32_t>{ 1 });
        CHECK(compiled.batches[3].queue == queue_type::compute);
        CHECK(compiled.batches[3].waits.empty());
        CHECK(compiled.batches[4].queue == queue_type::graphics);
        CHECK(compiled.batches[4].waits.empty());
        CHECK_FALSE(compiled.batches[3].signals);
    }

    SECTION("an async pass whose output nobody on graphics consumes adds no wait on graphics")
    {
        graph.add_compute_pass<empty_data>( // pass 2: keeps the async pass alive, on compute too
            "async consumer",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(async.output, buffer_read::srv_compute);
                builder.set_side_effects();
            },
            compute_execute,
            compute_queue_hint::async_compute);
        add_present(graph, "graphics", source, import_output(graph)); // pass 3

        const compiled_graph& compiled = compile(graph);
        REQUIRE(compiled.batches.size() == 3);
        CHECK(compiled.batches[1].queue == queue_type::compute);
        CHECK(compiled.batches[1].passes.size() == 2);
        CHECK(compiled.batches[2].queue == queue_type::graphics);
        CHECK(compiled.batches[2].waits.empty());
        CHECK_FALSE(compiled.batches[1].signals);
    }

    SECTION("a wait implied by another queue's wait is left out")
    {
        // copy waits for compute batch 1, which waited for graphics batch 0; a copy pass reading both needs one wait
        const buffer copy_output = import_buffer(graph, "copy output");
        const buffer_data& staged = graph.add_copy_pass<buffer_data>( // pass 2, on the copy queue
            "stage",
            [&](render_pass_builder& builder, buffer_data& data)
            {
                builder.read(async.output, buffer_read::copy_source);
                data.output = builder.write(copy_output, buffer_write::copy_destination);
            },
            copy_execute);
        graph.add_compute_pass<empty_data>( // pass 3, graphics
            "late consumer",
            [&](render_pass_builder& builder, empty_data&)
            {
                builder.read(staged.output, buffer_read::srv_compute);
                builder.read(async.output, buffer_read::srv_compute);
                builder.write(output, buffer_write::uav);
            },
            compute_execute);

        const compiled_graph& compiled = compile(graph);
        REQUIRE(compiled.batches.size() == 4);
        CHECK(compiled.batches[2].queue == queue_type::copy);
        CHECK(waits_of(compiled.batches[2]) == std::vector<std::uint32_t>{ 1 });
        // Waiting for the copy batch implies the compute batch it waited for
        CHECK(waits_of(compiled.batches[3]) == std::vector<std::uint32_t>{ 2 });
    }
}

TEST_CASE("render graph imports with ready_after make the first batch using them on each queue wait", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    const avis::rhi::synchronization_point uploaded{ queue_type::copy, 42 };
    const buffer vertices = import_buffer(graph, "vertices", uploaded);
    const buffer complete = import_buffer(graph, "complete", avis::rhi::synchronization_point{});
    const buffer output = import_buffer(graph, "output");

    graph.add_compute_pass<empty_data>( // batch 0, graphics
        "first user",
        [&](render_pass_builder& builder, empty_data&)
        {
            builder.read(vertices, buffer_read::srv_compute);
            builder.read(complete, buffer_read::srv_compute);
            builder.set_side_effects();
        },
        compute_execute);
    graph.add_compute_pass<empty_data>( // batch 1, compute
        "async user",
        [&](render_pass_builder& builder, empty_data&)
        {
            builder.read(vertices, buffer_read::srv_compute);
            builder.write(output, buffer_write::uav);
        },
        compute_execute,
        compute_queue_hint::async_compute);
    graph.add_compute_pass<empty_data>( // batch 2, graphics again: already waited on this queue
        "second user",
        [&](render_pass_builder& builder, empty_data&)
        {
            builder.read(vertices, buffer_read::srv_compute);
            builder.set_side_effects();
        },
        compute_execute);

    const compiled_graph& compiled = compile(graph);
    REQUIRE(compiled.batches.size() == 3);
    // A point with value 0 is always complete and adds no wait
    REQUIRE(compiled.batches[0].ready_after.size() == 1);
    CHECK(compiled.batches[0].ready_after[0] == uploaded);
    REQUIRE(compiled.batches[1].ready_after.size() == 1);
    CHECK(compiled.batches[1].ready_after[0] == uploaded);
    CHECK(compiled.batches[2].ready_after.empty());
}

TEST_CASE("render graph: compiling 100 passes takes under 0.2 ms", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    // A fixed pattern: each pass reads the two previous outputs and writes a new texture; every eleventh pass is an
    // async compute pass writing a buffer; one in seven outputs is unused (culled); the last pass writes the import
    const auto build = [&]
    {
        graph.reset();
        const texture output = import_output(graph);
        std::vector<texture> textures;
        std::vector<buffer> buffers;
        for (int index = 0; index < 99; ++index)
        {
            if (index % 11 == 10)
            {
                const buffer_data& data = graph.add_compute_pass<buffer_data>(
                    "async",
                    [&](render_pass_builder& builder, buffer_data& data)
                    {
                        builder.read(textures.back(), texture_read::srv_compute);
                        data.output = builder.write(builder.create_buffer("b", { .size = 4096 }), buffer_write::uav);
                    },
                    compute_execute,
                    compute_queue_hint::async_compute);
                buffers.push_back(data.output);
                continue;
            }
            const texture_data& data = graph.add_graphics_pass<texture_data>(
                "pass",
                [&](render_pass_builder& builder, texture_data& data)
                {
                    const std::size_t count = textures.size();
                    for (std::size_t back = 1; back <= 2 && back <= count; ++back)
                    {
                        if ((count - back) % 7 != 3)
                        {
                            builder.read(textures[count - back], texture_read::srv_pixel);
                        }
                    }
                    if (!buffers.empty() && index % 11 == 0)
                    {
                        builder.read(buffers.back(), buffer_read::srv_pixel);
                    }
                    data.output = builder.write(
                        builder.create_texture("t", { .size = view_relative(0.5f) }), texture_write::render_target);
                },
                graphics_execute);
            textures.push_back(data.output);
        }
        add_present(graph, "present", textures.back(), output);
    };

    std::vector<double> microseconds;
    for (int run = 0; run < 50; ++run)
    {
        build();
        const auto start = std::chrono::steady_clock::now();
        graph.compile();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        microseconds.push_back(std::chrono::duration<double, std::micro>(elapsed).count());
    }
    REQUIRE(graph.compiled() != nullptr);
    CHECK(graph.compiled()->order.size() > 50);
    CHECK(graph.compiled()->batches.size() > 2);

    std::ranges::sort(microseconds);
    const double median = microseconds[microseconds.size() / 2];
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(median < 200.0);
#else
    WARN(std::format("compiling 100 passes: median {:.1f} µs (checked in Release only)", median));
#endif
}

TEST_CASE("render graph debug information lists passes and resources", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;
    CHECK(graph.debug_information().passes.empty()); // before compile

    const texture output = import_output(graph);
    const texture a = add_producer(graph, "a").output;
    const texture b = add_producer(graph, "b", std::span{ &a, 1 }).output;
    const texture unused = add_producer(graph, "unused", std::span{ &a, 1 }).output;
    add_present(graph, "present", b, output);

    graph.compile();
    const graph_debug_information& information = graph.debug_information();

    REQUIRE(information.passes.size() == 4);
    CHECK(information.passes[0].name == "a");
    CHECK(information.passes[2].name == "unused");
    CHECK(information.passes[2].culled);
    CHECK_FALSE(information.passes[3].culled);
    CHECK(information.passes[3].queue == queue_type::graphics);
    CHECK_FALSE(information.passes[0].gpu_milliseconds.has_value());

    const auto list = [](const std::span<const std::uint32_t> passes)
    { return std::vector<std::uint32_t>{ passes.begin(), passes.end() }; };
    const resource_debug_information& first = information.resources[a.index()];
    CHECK(first.name == "a");
    CHECK(first.is_texture);
    CHECK(first.texture.size == absolute(16, 16));
    CHECK(list(first.producers) == std::vector<std::uint32_t>{ 0 });
    CHECK(list(first.consumers) == std::vector<std::uint32_t>{ 1, 2 });
    CHECK(first.first_use == 0);
    CHECK(first.last_use == 1); // the culled reader isn't a use
    CHECK(first.heap_offset != resource_debug_information::no_offset);

    const resource_debug_information& culled = information.resources[unused.index()];
    CHECK(list(culled.producers) == std::vector<std::uint32_t>{ 2 });
    CHECK(culled.consumers.empty());
    CHECK(culled.first_use == resource_debug_information::no_position);
    CHECK(culled.heap_offset == resource_debug_information::no_offset);

    const resource_debug_information& imported = information.resources[output.index()];
    CHECK(imported.imported);
    CHECK(list(imported.producers) == std::vector<std::uint32_t>{ 3 });
    CHECK(imported.first_use == 2);
    CHECK(imported.last_use == 2);
    CHECK(information.transient_heap_size > 0);

    graph.reset();
    CHECK(graph.debug_information().passes.empty());
}

TEST_CASE("render graph debug information: the debug selection isn't aliased", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;
    const bool selected = GENERATE(false, true);

    // x and y have disjoint lifetimes: they share memory unless x is the debug view's selection
    const texture first_output = import_output(graph, "first");
    const texture second_output = import_output(graph, "second");
    const texture x = add_producer(graph, "x").output;
    graph.add_graphics_pass<empty_data>(
        "show x",
        [&](render_pass_builder& builder, empty_data&)
        {
            if (selected)
            {
                builder.read_for_debug(x, texture_read::srv_pixel);
            }
            else
            {
                builder.read(x, texture_read::srv_pixel);
            }
            builder.write(first_output, texture_write::render_target);
        },
        graphics_execute);
    const texture y = add_producer(graph, "y").output;
    add_present(graph, "show y", y, second_output);

    graph.compile();
    const graph_debug_information& information = graph.debug_information();
    const resource_debug_information& selection = information.resources[x.index()];
    const resource_debug_information& other = information.resources[y.index()];
    CHECK(selection.debug_selected == selected);
    CHECK_FALSE(information.passes[1].culled); // the read keeps its producer
    CHECK_FALSE(information.passes[0].culled);
    if (selected)
    {
        CHECK_FALSE(selection.aliased);
        CHECK(selection.heap_offset != other.heap_offset);
    }
    else
    {
        CHECK(selection.aliased);
        CHECK(selection.heap_offset == other.heap_offset);
    }
}
