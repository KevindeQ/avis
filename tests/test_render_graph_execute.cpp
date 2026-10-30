// Executing render graphs (docs/tasks/09-render-graph.md §9.8–9.9). So far the materialise step: the resource bag's
// contents and checks, and frame-memory exhaustion. On the test RHI, with frames of the fixture's scheduler.

#include "helpers/recorded_assertions.h"
#include "helpers/render_graph_fixture.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/render_graph_executor.h" // private, lib/avis/graphics/
#include "avis/graphics/resource_bag.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/error.h"
#include "avis/rhi/formats.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <vector>

using namespace avis::graphics;
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

TEST_CASE("render graph materialise: running out of frame descriptors fails before recording", "[render_graph]")
{
    render_graph_fixture fixture{ {}, 16 };
    render_graph& graph = fixture.graph;

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

    frame_token frame = begin_frame(fixture);
    const auto bag = graph.materialise(frame);
    REQUIRE_FALSE(bag.has_value());
    CHECK(rhi::is_error(bag.error(), rhi::error_kind::transient_memory_exhausted));
    fixture.frames->end_frame(std::move(frame), {});

    // The next frame works
    graph.reset();
    const texture next_output = import_output(graph);
    add_chain(graph, next_output, { .size = absolute(8, 8) });
    graph.compile();
    frame_token next = begin_frame(fixture);
    CHECK(graph.materialise(next).has_value());
    fixture.frames->end_frame(std::move(next), {});
}
