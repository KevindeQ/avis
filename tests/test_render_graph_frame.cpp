// Whole render graph frames on the GPU (docs/tasks/09-render-graph.md §9.11): the minimal frame of 10 §3.6, async
// compute, aliasing, multi-view and a chain of frames. With the debug layer and GPU-based validation on; the RHI
// fixture fails a test on any validation error. Results come back through the readback queue (task 7) or readback
// buffers.

#include "helpers/graphics_fixture.h"
#include "helpers/render_graph_fixture.h"
#include "helpers/runtime_fixture.h"
#include "rhi_test_compute_shader.h"
#include "rhi_test_pixel_shader.h"
#include "rhi_test_vertex_shader.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/graph_debug_information.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/readback_queue.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/render_graph_compiler.h" // private, lib/avis/graphics/
#include "avis/graphics/resource_bag.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/d3d12/command_list_pool.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/runtime.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

using namespace avis::graphics;
using avis::rhi::queue_type;
using avis::tests::render_graph_fixture;
namespace rhi = avis::rhi;

namespace
{
    using pixel = std::array<std::uint8_t, 4>;
    constexpr pixel red{ 255, 0, 0, 255 };
    constexpr pixel green{ 0, 255, 0, 255 };
    constexpr pixel blue{ 0, 0, 255, 255 };

    template<std::size_t Size>
    std::span<const std::byte> bytes(const unsigned char (&code)[Size]) noexcept
    {
        return std::as_bytes(std::span{ code });
    }

    std::array<float, 4> color_of(const pixel value)
    {
        return { value[0] / 255.0f, value[1] / 255.0f, value[2] / 255.0f, value[3] / 255.0f };
    }

    avis::tests::render_graph_fixture_options validated(const render_graph_configuration& graph = {})
    {
        return { .graph = graph, .gpu_based_validation = true };
    }

    // The RHI tests' triangle (tests/shaders/rhi_test_triangle.hlsl): covers the viewport with a root-constant color
    rhi::graphics_pipeline_handle make_triangle_pipeline(render_graph_fixture& fixture)
    {
        rhi::pipelines pipelines{ *fixture.hardware_interface };
        const std::array parameters{ rhi::root_parameter{ .kind = rhi::root_parameter_kind::constants,
                                                          .shader_register = 0,
                                                          .constant_count = 4,
                                                          .visibility = rhi::shader_visibility::pixel } };
        auto signature = pipelines.create_root_signature({ .parameters = parameters, .debug_name = "triangle" });
        REQUIRE(signature.has_value());
        rhi::graphics_pipeline_description description{ .root_signature = *signature,
                                                        .vertex_shader = bytes(rhi_test_vertex_shader),
                                                        .pixel_shader = bytes(rhi_test_pixel_shader),
                                                        .debug_name = "triangle" };
        description.state.rasterizer.cull = rhi::cull_mode::none;
        description.state.depth.test = false;
        description.state.render_target_count = 1;
        description.state.render_target_formats[0] = rhi::format::rgba8_unorm;
        auto pipeline = pipelines.create_pipeline(description);
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    // The RHI tests' fill shader (tests/shaders/rhi_test_fill.hlsl): element i of a raw UAV buffer = base + i
    rhi::compute_pipeline_handle make_fill_pipeline(render_graph_fixture& fixture)
    {
        rhi::pipelines pipelines{ *fixture.hardware_interface };
        const std::array parameters{ rhi::root_parameter{
            .kind = rhi::root_parameter_kind::constants, .shader_register = 0, .constant_count = 3 } };
        auto signature = pipelines.create_root_signature({ .parameters = parameters, .debug_name = "fill" });
        REQUIRE(signature.has_value());
        auto pipeline = pipelines.create_pipeline(
            rhi::compute_pipeline_description{
                .root_signature = *signature, .compute_shader = bytes(rhi_test_compute_shader), .debug_name = "fill" });
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    rhi::texture_handle
        make_output(render_graph_fixture& fixture, const std::uint32_t width, const std::uint32_t height)
    {
        auto texture = rhi::resources{ *fixture.hardware_interface }.create_texture(
            { .width = width,
              .height = height,
              .format = rhi::format::rgba8_unorm,
              .usage = rhi::texture_usage::copy_destination | rhi::texture_usage::copy_source,
              .initial_layout = rhi::texture_layout::common,
              .debug_name = "output" });
        REQUIRE(texture.has_value());
        return *texture;
    }

    rhi::buffer_handle make_readback(render_graph_fixture& fixture, const std::uint64_t size)
    {
        auto buffer = rhi::resources{ *fixture.hardware_interface }.create_buffer(
            { .size = size,
              .usage = rhi::buffer_usage::copy_destination,
              .memory = rhi::memory_type::readback,
              .debug_name = "readback" });
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    // Builds a frame with `build`, compiles, executes and waits for it on the GPU
    submitted_frame run_frame(render_graph_fixture& fixture, const std::function<void(render_graph&)>& build)
    {
        fixture.graph.reset();
        frame_token frame = fixture.begin_frame();
        build(fixture.graph);
        fixture.graph.compile();
        const auto submitted = fixture.execute(frame);
        REQUIRE(submitted.has_value());
        fixture.wait(*submitted);
        fixture.frames->end_frame(std::move(frame), submitted->last_submissions());
        return *submitted;
    }

    // A readback_texture_sender driven by the test: completes from poll_completions()
    struct image_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value(readback_image value) noexcept
        {
            image->emplace(std::move(value));
        }

        void set_error(avis::error) noexcept {}
        void set_stopped() noexcept {}

        stdexec::env<> get_env() const noexcept
        {
            return {};
        }

        std::optional<readback_image>* image;
    };

    readback_image
        read_texture(render_graph_fixture& fixture, readback_queue& readbacks, const rhi::texture_handle texture)
    {
        std::optional<readback_image> image;
        auto operation = stdexec::connect(readbacks.read_texture(texture), image_receiver{ &image });
        stdexec::start(operation);
        fixture.hardware_interface.wait_for(
            rhi::queue<queue_type::graphics>{ *fixture.hardware_interface }.last_submitted());
        REQUIRE(image.has_value());
        return std::move(*image);
    }

    pixel pixel_at(const readback_image& image, const std::uint32_t x, const std::uint32_t y)
    {
        pixel value{};
        std::memcpy(value.data(), image.bytes().data() + y * image.row_pitch() + x * 4, 4);
        return value;
    }

    bool filled_with(const readback_image& image, const pixel value)
    {
        for (std::uint32_t y = 0; y < image.height(); ++y)
        {
            for (std::uint32_t x = 0; x < image.width(); ++x)
            {
                if (pixel_at(image, x, y) != value)
                {
                    return false;
                }
            }
        }
        return true;
    }

    struct target_data
    {
        texture target;
    };

    struct copy_data
    {
        texture source;
        texture destination;
    };

    // A pass that creates a transient render target and clears it
    const target_data&
        add_clear(render_graph& graph, const std::string_view name, const texture_size size, const pixel color)
    {
        return graph.add_graphics_pass<target_data>(
            name,
            [&](render_pass_builder& builder, target_data& data)
            {
                data.target = builder.write(
                    builder.create_texture(
                        "color",
                        { .size = size,
                          .format = rhi::format::rgba8_unorm,
                          .clear_value = rhi::clear_value{ .color = color_of(color) } }),
                    texture_write::render_target);
            },
            [color](const target_data& data, const resource_bag& resources, graphics_pass_context& context)
            {
                const std::array<float, 4> value = color_of(color);
                context.recorder().clear_render_target(resources.render_target_view(data.target), value);
            });
    }

    // The present pass of 10 §3.6: copies a transient texture into an import
    void add_present(render_graph& graph, const texture source, const texture output)
    {
        graph.add_copy_pass<copy_data>(
            "present",
            [&](render_pass_builder& builder, copy_data& data)
            {
                data.source = builder.read(source, texture_read::copy_source);
                data.destination = builder.write(output, texture_write::copy_destination);
            },
            [](const copy_data& data, const resource_bag& resources, copy_pass_context& context)
            {
                context.recorder().copy_texture(
                    resources.texture_handle(data.destination), {}, resources.texture_handle(data.source), {});
            });
    }

    texture import_output(render_graph& graph, const rhi::texture_handle output)
    {
        return graph.import_texture("output", output, rhi::access_states::common, rhi::access_states::common);
    }
} // namespace

TEST_CASE("render graph frame: the minimal frame of 10 §3.6", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture{ validated() };
    const auto readbacks = avis::tests::make_test_readback_queue(fixture.hardware_interface);
    const rhi::graphics_pipeline_handle triangle_pipeline = make_triangle_pipeline(fixture);
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    const rhi::texture_handle output_texture = make_output(fixture, width, height);
    fixture.graph.set_view_size({ width, height });

    run_frame(
        fixture,
        [&](render_graph& graph)
        {
            const texture output = import_output(graph, output_texture);
            const target_data& triangle = graph.add_graphics_pass<target_data>(
                "triangle",
                [&](render_pass_builder& builder, target_data& data)
                {
                    const texture color = builder.create_texture(
                        "color",
                        { .size = view_relative(1.0f),
                          .format = rhi::format::rgba8_unorm,
                          .clear_value = rhi::clear_value{ .color = color_of(blue) } });
                    data.target = builder.write(color, texture_write::render_target);
                    builder.set_render_area(width, height);
                },
                [triangle_pipeline](
                    const target_data& data, const resource_bag& resources, graphics_pass_context& context)
                {
                    rhi::graphics_recorder& recorder = context.recorder();
                    const rhi::texture_view target = resources.render_target_view(data.target);
                    recorder.clear_render_target(target, color_of(blue));
                    recorder.set_render_targets(std::span{ &target, 1 });
                    // The left half: the triangle covers its viewport
                    recorder.set_viewport(
                        { .width = context.render_width() / 2.0f,
                          .height = static_cast<float>(context.render_height()) });
                    recorder.set_scissor(
                        { .right = static_cast<std::int32_t>(context.render_width()),
                          .bottom = static_cast<std::int32_t>(context.render_height()) });
                    recorder.set_pipeline(triangle_pipeline);
                    recorder.set_root_constants(0, color_of(red));
                    recorder.draw(3);
                });
            add_present(graph, triangle.target, output);
        });

    const graph_debug_information& information = fixture.graph.debug_information();
    REQUIRE(information.passes.size() == 2);
    CHECK(information.passes[1].queue == queue_type::graphics); // color isn't in common

    const readback_image image = read_texture(fixture, *readbacks, output_texture);
    CHECK(pixel_at(image, 5, 16) == red); // drawn
    CHECK(pixel_at(image, 60, 16) == blue); // cleared

    rhi::resources{ *fixture.hardware_interface }.destroy(output_texture);
}

TEST_CASE("render graph frame: async compute output copied on graphics", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture{ validated() };
    const rhi::compute_pipeline_handle fill_pipeline = make_fill_pipeline(fixture);
    constexpr std::uint32_t count = 64;
    constexpr std::uint32_t base = 1000;
    const rhi::buffer_handle readback = make_readback(fixture, count * 4);

    struct fill_data
    {
        buffer target;
    };
    struct buffer_copy_data
    {
        buffer source;
        buffer destination;
    };

    run_frame(
        fixture,
        [&](render_graph& graph)
        {
            const buffer output =
                graph.import_buffer("output", readback, rhi::access_states::common, rhi::access_states::common);
            const fill_data& fill = graph.add_compute_pass<fill_data>(
                "fill",
                [&](render_pass_builder& builder, fill_data& data)
                {
                    data.target = builder.write(
                        builder.create_buffer("values", { .size = count * 4, .raw = true }), buffer_write::uav);
                },
                [fill_pipeline](const fill_data& data, const resource_bag& resources, compute_pass_context& context)
                {
                    rhi::compute_recorder& recorder = context.recorder();
                    recorder.set_pipeline(fill_pipeline);
                    recorder.set_root_constants(
                        0, std::array<std::uint32_t, 3>{ resources.uav(data.target).value, base, count });
                    recorder.dispatch((count + 63) / 64, 1, 1);
                },
                compute_queue_hint::async_compute);
            graph.add_graphics_pass<buffer_copy_data>(
                "copy",
                [&](render_pass_builder& builder, buffer_copy_data& data)
                {
                    data.source = builder.read(fill.target, buffer_read::copy_source);
                    data.destination = builder.write(output, buffer_write::copy_destination);
                },
                [](const buffer_copy_data& data, const resource_bag& resources, graphics_pass_context& context)
                {
                    context.recorder().copy_buffer(
                        resources.buffer_handle(data.destination),
                        0,
                        resources.buffer_handle(data.source),
                        0,
                        count * 4);
                });
        });

    const detail::compiled_graph& compiled = *fixture.graph.compiled();
    REQUIRE(compiled.batches.size() == 2);
    CHECK(compiled.batches[0].queue == queue_type::compute);
    CHECK(compiled.batches[0].waits.empty());
    CHECK(compiled.batches[1].waits.size() == 1); // the one cross-queue wait

    const std::span<std::byte> values = rhi::resources{ *fixture.hardware_interface }.mapped_bytes(readback);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        std::uint32_t value = 0;
        std::memcpy(&value, values.data() + index * 4, 4);
        INFO("element " << index);
        CHECK(value == base + index);
    }
    rhi::resources{ *fixture.hardware_interface }.destroy(readback);
}

TEST_CASE("render graph frame: two transient render targets share memory", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture{ validated() };
    const auto readbacks = avis::tests::make_test_readback_queue(fixture.hardware_interface);
    const rhi::texture_handle first_output = make_output(fixture, 64, 32);
    const rhi::texture_handle second_output = make_output(fixture, 32, 32);

    texture first;
    texture second;
    run_frame(
        fixture,
        [&](render_graph& graph)
        {
            const texture first_import =
                graph.import_texture("first", first_output, rhi::access_states::common, rhi::access_states::common);
            const texture second_import =
                graph.import_texture("second", second_output, rhi::access_states::common, rhi::access_states::common);
            // Different sizes: two placed resources, at one offset
            first = add_clear(graph, "red", absolute(64, 32), red).target;
            add_present(graph, first, first_import);
            second = add_clear(graph, "green", absolute(32, 32), green).target;
            add_present(graph, second, second_import);
        });

    const graph_debug_information& information = fixture.graph.debug_information();
    const resource_debug_information& first_information = information.resources[first.index()];
    const resource_debug_information& second_information = information.resources[second.index()];
    CHECK(first_information.aliased);
    CHECK(second_information.aliased);
    CHECK(first_information.heap_offset == second_information.heap_offset);

    CHECK(filled_with(read_texture(fixture, *readbacks, first_output), red));
    CHECK(filled_with(read_texture(fixture, *readbacks, second_output), green));

    rhi::resources{ *fixture.hardware_interface }.destroy(first_output);
    rhi::resources{ *fixture.hardware_interface }.destroy(second_output);
}

TEST_CASE("render graph frame: two views run the same pass functions", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture{ validated() };
    const auto readbacks = avis::tests::make_test_readback_queue(fixture.hardware_interface);
    const rhi::texture_handle main_output = make_output(fixture, 64, 32);
    const rhi::texture_handle topdown_output = make_output(fixture, 32, 32);

    // The renderer's per-view chain: clear a view-sized target and present it into the view's output
    const auto add_view = [](render_graph& graph, const rhi::texture_handle output_texture, const pixel color)
    {
        const texture output = import_output(graph, output_texture);
        const target_data& cleared = add_clear(graph, "clear", view_relative(1.0f), color);
        add_present(graph, cleared.target, output);
    };

    run_frame(
        fixture,
        [&](render_graph& graph)
        {
            {
                const view_scope scope = graph.begin_view("main", { 64, 32 });
                add_view(graph, main_output, red);
            }
            {
                const view_scope scope = graph.begin_view("topdown", { 32, 32 });
                add_view(graph, topdown_output, green);
            }
        });

    const graph_debug_information& information = fixture.graph.debug_information();
    REQUIRE(information.passes.size() == 4);
    CHECK(information.passes[0].name == "main/clear");
    CHECK(information.passes[3].name == "topdown/present");
    const auto has_resource = [&](const std::string_view name)
    {
        return std::ranges::any_of(
            information.resources, [&](const resource_debug_information& resource) { return resource.name == name; });
    };
    CHECK(has_resource("main/color"));
    CHECK(has_resource("main/output"));
    CHECK(has_resource("topdown/color"));
    CHECK(has_resource("topdown/output"));

    const readback_image main_image = read_texture(fixture, *readbacks, main_output);
    const readback_image topdown_image = read_texture(fixture, *readbacks, topdown_output);
    CHECK(main_image.width() == 64);
    CHECK(filled_with(main_image, red));
    CHECK(filled_with(topdown_image, green));

    rhi::resources{ *fixture.hardware_interface }.destroy(main_output);
    rhi::resources{ *fixture.hardware_interface }.destroy(topdown_output);
}

TEST_CASE("render graph frame: a chain of 100 frames reaches a steady state", "[render_graph]")
{
    avis::runtime runtime{ avis::tests::small_runtime_configuration() };
    render_graph_fixture fixture{ validated() };
    rhi::d3d12::device_state& state = fixture.hardware_interface->state();
    const rhi::compute_pipeline_handle fill_pipeline = make_fill_pipeline(fixture);
    constexpr std::uint32_t frames_in_flight = 2;
    constexpr std::uint32_t count = 64;
    const rhi::texture_handle output_texture = make_output(fixture, 32, 32);
    const std::array readbacks{ make_readback(fixture, count * 4), make_readback(fixture, count * 4) };

    struct fill_data
    {
        buffer target;
    };
    struct buffer_copy_data
    {
        buffer source;
        buffer destination;
    };

    // Frame N − F's submissions, waited for before frame N begins
    std::deque<submitted_frame> in_flight;
    struct counters
    {
        transient_pool_statistics pool;
        std::uint32_t textures = 0;
        std::uint32_t buffers = 0;
        std::size_t allocators = 0;
    };
    const auto snapshot = [&]
    {
        return counters{ .pool = fixture.pool.statistics(),
                         .textures = state.resources().texture_slots_used(),
                         .buffers = state.resources().buffer_slots_used(),
                         .allocators = state.command_lists().allocators_created() };
    };
    std::optional<counters> after_first_frames;
    std::optional<counters> warmed_up;

    for (std::uint32_t frame_number = 0; frame_number < 100; ++frame_number)
    {
        if (in_flight.size() == frames_in_flight)
        {
            fixture.wait(in_flight.front());
            in_flight.pop_front();
        }
        render_graph& graph = fixture.graph;
        graph.reset();
        frame_token frame = fixture.begin_frame();

        const std::uint32_t base = frame_number * 100;
        const texture output = import_output(graph, output_texture);
        const buffer values_output = graph.import_buffer(
            "values", readbacks[frame_number % 2], rhi::access_states::common, rhi::access_states::common);
        add_present(
            graph, add_clear(graph, "clear", absolute(32, 32), frame_number % 2 == 0 ? red : green).target, output);
        const fill_data& fill = graph.add_compute_pass<fill_data>(
            "fill",
            [&](render_pass_builder& builder, fill_data& data)
            {
                data.target = builder.write(
                    builder.create_buffer("values", { .size = count * 4, .raw = true }), buffer_write::uav);
            },
            [fill_pipeline, base](const fill_data& data, const resource_bag& resources, compute_pass_context& context)
            {
                context.recorder().set_pipeline(fill_pipeline);
                context.recorder().set_root_constants(
                    0, std::array<std::uint32_t, 3>{ resources.uav(data.target).value, base, count });
                context.recorder().dispatch(1, 1, 1);
            },
            compute_queue_hint::async_compute);
        graph.add_graphics_pass<buffer_copy_data>(
            "copy values",
            [&](render_pass_builder& builder, buffer_copy_data& data)
            {
                data.source = builder.read(fill.target, buffer_read::copy_source);
                data.destination = builder.write(values_output, buffer_write::copy_destination);
            },
            [](const buffer_copy_data& data, const resource_bag& resources, graphics_pass_context& context)
            {
                context.recorder().copy_buffer(
                    resources.buffer_handle(data.destination), 0, resources.buffer_handle(data.source), 0, count * 4);
            });
        graph.compile();

        const auto submitted = fixture.execute(frame);
        REQUIRE(submitted.has_value());
        CHECK(submitted->count == 2);
        fixture.frames->end_frame(std::move(frame), submitted->last_submissions());
        in_flight.push_back(*submitted);

        if (frame_number + 1 == frames_in_flight)
        {
            after_first_frames = snapshot();
        }
        // Which worker records which queue's group varies, so allocators get a few frames longer to warm up
        if (frame_number + 1 == 10)
        {
            warmed_up = snapshot();
        }
    }
    while (!in_flight.empty())
    {
        fixture.wait(in_flight.front());
        in_flight.pop_front();
    }

    // The last frame's values (frame 99, odd: the second readback)
    const std::span<std::byte> values = rhi::resources{ *fixture.hardware_interface }.mapped_bytes(readbacks[1]);
    std::uint32_t first_value = 0;
    std::memcpy(&first_value, values.data(), 4);
    CHECK(first_value == 9900);

    const counters end = snapshot();
    REQUIRE(after_first_frames.has_value());
    REQUIRE(warmed_up.has_value());
    CHECK(end.pool.textures_created == after_first_frames->pool.textures_created);
    CHECK(end.pool.buffers_created == after_first_frames->pool.buffers_created);
    CHECK(end.pool.heaps_created == after_first_frames->pool.heaps_created);
    CHECK(end.pool.textures_created == frames_in_flight);
    CHECK(end.textures == after_first_frames->textures);
    CHECK(end.buffers == after_first_frames->buffers);
    CHECK(end.allocators == warmed_up->allocators);

    rhi::resources{ *fixture.hardware_interface }.destroy(output_texture);
    rhi::resources{ *fixture.hardware_interface }.destroy(readbacks[0]);
    rhi::resources{ *fixture.hardware_interface }.destroy(readbacks[1]);
}
