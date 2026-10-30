// Building render graphs (docs/tasks/09-render-graph.md): virtual resources, descriptions and usages (§9.2) without a
// device; the build API, pass storage and view scopes (§9.3) on a graph over the test RHI, which executes nothing.

#include "helpers/log_capture.h"
#include "helpers/recorded_assertions.h"
#include "helpers/render_graph_fixture.h"

#include "avis/graphics/buffer.h"
#include "avis/graphics/pass_context.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/render_graph_tables.h" // private, lib/avis/graphics/
#include "avis/graphics/resource.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/command_list.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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

// ---- §9.3: build API, pass storage, view scopes ----

namespace
{
    using avis::graphics::detail::render_graph_tables;
    using avis::tests::render_graph_fixture;

    struct empty_data
    {};

    struct texture_data
    {
        texture color;
    };

    // Callables of every context type, for the requires checks and as no-op execute callbacks
    constexpr auto no_setup = [](render_pass_builder&, empty_data&) {};
    constexpr auto graphics_execute = [](const empty_data&, const resource_bag&, graphics_pass_context&) {};
    constexpr auto compute_execute = [](const empty_data&, const resource_bag&, compute_pass_context&) {};
    constexpr auto copy_execute = [](const empty_data&, const resource_bag&, copy_pass_context&) {};

    template<typename Execute>
    concept graphics_pass_accepts = requires(render_graph& graph, const Execute& execute) {
        graph.add_graphics_pass<empty_data>("pass", no_setup, execute);
    };
    template<typename Execute>
    concept compute_pass_accepts = requires(render_graph& graph, const Execute& execute) {
        graph.add_compute_pass<empty_data>("pass", no_setup, execute);
    };
    template<typename Execute>
    concept copy_pass_accepts = requires(render_graph& graph, const Execute& execute) {
        graph.add_copy_pass<empty_data>("pass", no_setup, execute);
    };

    // The add function fixes the context type (10 §3.1)
    static_assert(graphics_pass_accepts<decltype(graphics_execute)>);
    static_assert(compute_pass_accepts<decltype(compute_execute)>);
    static_assert(copy_pass_accepts<decltype(copy_execute)>);
    static_assert(!compute_pass_accepts<decltype(graphics_execute)>);
    static_assert(!graphics_pass_accepts<decltype(compute_execute)>);
    static_assert(!copy_pass_accepts<decltype(compute_execute)>);

    // A compute context's recorder can't draw; a graphics one can
    template<typename Recorder>
    concept can_draw = requires(Recorder& recorder) { recorder.draw(3u); };
    static_assert(
        std::is_same_v<decltype(std::declval<compute_pass_context&>().recorder()), avis::rhi::compute_recorder&>);
    static_assert(!can_draw<avis::rhi::compute_recorder>);
    static_assert(!can_draw<avis::rhi::copy_recorder>);
    static_assert(can_draw<avis::rhi::graphics_recorder>);

    // Copy contexts can't allocate constants
    template<typename Context>
    concept can_allocate_constants = requires(Context& context, const std::uint32_t value) {
        context.allocate_constants(value);
        context.allocate_constants(std::span<const std::byte>{});
    };
    static_assert(can_allocate_constants<graphics_pass_context>);
    static_assert(can_allocate_constants<compute_pass_context>);
    static_assert(!can_allocate_constants<copy_pass_context>);

    // Not copyable or movable: passes keep pointers to the graph
    static_assert(!std::is_copy_constructible_v<render_graph> && !std::is_move_constructible_v<render_graph>);

    std::string_view resource_name(const render_graph& graph, const resource& handle)
    {
        return graph.tables().resources[handle.index()].name;
    }

    std::string_view pass_name(const render_graph& graph, const std::size_t pass)
    {
        return graph.tables().passes[pass]->name;
    }
} // namespace

TEST_CASE("render_graph: setup runs inside add_*_pass, in declaration order, on the calling thread", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;
    const std::thread::id caller = std::this_thread::get_id();

    struct counter_data
    {
        int value = 0;
    };
    const auto no_execute = [](const counter_data&, const resource_bag&, graphics_pass_context&) {};

    std::vector<int> calls;
    std::vector<std::thread::id> threads;
    const counter_data& first = graph.add_graphics_pass<counter_data>(
        "first",
        [&](render_pass_builder&, counter_data& data)
        {
            calls.push_back(1);
            threads.push_back(std::this_thread::get_id());
            data.value = 42;
        },
        no_execute);
    CHECK(calls == std::vector<int>{ 1 });

    graph.add_compute_pass<empty_data>(
        "second",
        [&](render_pass_builder&, empty_data&)
        {
            calls.push_back(2);
            threads.push_back(std::this_thread::get_id());
        },
        compute_execute);
    graph.add_copy_pass<empty_data>(
        "third",
        [&](render_pass_builder&, empty_data&)
        {
            calls.push_back(3);
            threads.push_back(std::this_thread::get_id());
        },
        copy_execute);
    CHECK(calls == std::vector<int>{ 1, 2, 3 });
    CHECK(std::ranges::all_of(threads, [&](const std::thread::id thread) { return thread == caller; }));

    // The returned reference stays put while the arena grows
    const counter_data* const address = &first;
    for (int index = 0; index < 1000; ++index)
    {
        const counter_data& data = graph.add_graphics_pass<counter_data>(
            std::format("pass {}", index),
            [index](render_pass_builder&, counter_data& data) { data.value = index; },
            no_execute);
        CHECK(data.value == index);
    }
    CHECK(&first == address);
    CHECK(first.value == 42);

    const render_graph_tables& tables = graph.tables();
    REQUIRE(tables.passes.size() == 1003);
    CHECK(pass_name(graph, 0) == "first");
    CHECK(tables.passes[1]->type == pass_type::compute);
    CHECK(tables.passes[2]->type == pass_type::copy);
    CHECK(pass_name(graph, 1002) == "pass 999");
    CHECK(tables.passes[0]->data == address);
    CHECK(tables.passes[0]->execute_graphics != nullptr);
    CHECK(tables.passes[1]->execute_compute != nullptr);
    CHECK(tables.passes[2]->execute_copy != nullptr);
}

TEST_CASE("render_graph: writes make new versions and reads record edges", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    texture created;
    const texture_data& producer = graph.add_graphics_pass<texture_data>(
        "producer",
        [&](render_pass_builder& builder, texture_data& data)
        {
            created = builder.create_texture("color", { .size = absolute(64, 64) });
            data.color = builder.write(created, texture_write::render_target);
        },
        [](const texture_data&, const resource_bag&, graphics_pass_context&) {});
    CHECK(created.valid());
    CHECK(created.version() == 0);
    CHECK(producer.color.index() == created.index());
    CHECK(producer.color.version() == 1);

    graph.add_graphics_pass<empty_data>(
        "consumer",
        [&](render_pass_builder& builder, empty_data&)
        {
            CHECK(builder.read(producer.color, texture_read::srv_pixel) == producer.color);
            builder.set_side_effects();
        },
        graphics_execute);

    const render_graph_tables& tables = graph.tables();
    CHECK(std::ranges::any_of(tables.edges, [](const auto& edge) { return edge.producer == 0 && edge.consumer == 1; }));
    REQUIRE(tables.passes[1]->accesses.size() == 1);
    CHECK(tables.versions[tables.passes[1]->accesses[0].input].producer == 0);
    CHECK(tables.passes[1]->side_effects);

    SECTION("writing a version that was already written fires the check")
    {
        const avis::tests::recorded_assertions assertions;
        const avis::tests::log_capture capture{ "graphics" };

        texture rewritten;
        graph.add_graphics_pass<empty_data>(
            "stale writer",
            [&](render_pass_builder& builder, empty_data&)
            { rewritten = builder.write(created, texture_write::render_target); },
            graphics_execute);
        // Nothing is recorded: the handle comes back unchanged
        CHECK(rewritten == created);
        CHECK(tables.passes[2]->accesses.empty());
#ifndef NDEBUG
        CHECK(assertions.contains("already written"));
#endif
    }

    SECTION("a handle of an earlier frame fires the check")
    {
        // The pass data goes with the frame; the handle is a value
        const texture stale = producer.color;
        graph.reset();
        // The new frame has a resource at the same index, so only the frame serial tells them apart
        graph.add_graphics_pass<texture_data>(
            "new producer",
            [&](render_pass_builder& builder, texture_data& data)
            {
                data.color = builder.write(
                    builder.create_texture("color", { .size = absolute(64, 64) }), texture_write::render_target);
            },
            [](const texture_data&, const resource_bag&, graphics_pass_context&) {});

        const avis::tests::recorded_assertions assertions;
        const avis::tests::log_capture capture{ "graphics" };
        graph.add_graphics_pass<empty_data>(
            "stale reader",
            [&](render_pass_builder& builder, empty_data&) { builder.read(stale, texture_read::srv_pixel); },
            graphics_execute);
#ifndef NDEBUG
        CHECK(assertions.contains("another frame"));
        CHECK(graph.tables().passes[1]->accesses.empty());
#endif
    }
}

TEST_CASE("render_graph: imports are version-0 entries with their states", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    const avis::rhi::synchronization_point uploaded{ queue_type::copy, 7 };
    const texture output = graph.import_texture(
        "output",
        avis::rhi::texture_handle{},
        avis::rhi::access_states::common,
        avis::rhi::access_states::shader_resource);
    const buffer data = graph.import_buffer(
        "data",
        avis::rhi::buffer_handle{},
        avis::rhi::access_states::common,
        avis::rhi::access_states::common,
        uploaded);
    CHECK(output.version() == 0);
    CHECK(data.version() == 0);

    const auto& texture_entry = graph.tables().resources[output.index()];
    CHECK(texture_entry.imported);
    CHECK(texture_entry.name == "output");
    CHECK(texture_entry.creator == avis::graphics::detail::no_index);
    CHECK(texture_entry.final_state.layout == texture_layout::shader_resource);
    CHECK_FALSE(texture_entry.ready_after.has_value());

    const auto& buffer_entry = graph.tables().resources[data.index()];
    CHECK(buffer_entry.imported);
    CHECK(buffer_entry.kind == avis::graphics::detail::resource_kind::buffer);
    REQUIRE(buffer_entry.ready_after.has_value());
    CHECK(*buffer_entry.ready_after == uploaded);
}

TEST_CASE("render_graph: pass data and execute callables are destroyed on reset", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    const auto payload = std::make_shared<int>(1);
    struct named_data
    {
        std::string name;
        std::shared_ptr<int> payload;
    };

    const named_data& data = graph.add_compute_pass<named_data>(
        "named",
        [&](render_pass_builder&, named_data& data)
        {
            data.name = std::string(200, 'n'); // beyond the small-string buffer
            data.payload = payload;
        },
        [captured = payload](const named_data&, const resource_bag&, compute_pass_context&) { (void)captured; });
    CHECK(data.name.size() == 200);
    CHECK(payload.use_count() == 3); // the test, the pass data and the callable

    graph.reset();
    CHECK(payload.use_count() == 1);
    CHECK(graph.tables().passes.empty());
}

TEST_CASE("render_graph: view scopes qualify names", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;

    texture shared_texture;
    graph.add_graphics_pass<empty_data>(
        "shared",
        [&](render_pass_builder& builder, empty_data&)
        { shared_texture = builder.create_texture("instances", { .size = absolute(4, 4) }); },
        graphics_execute);

    texture view_texture;
    texture view_output;
    {
        const view_scope scope = graph.begin_view("main");
        CHECK(scope.name() == "main");
        view_output = graph.import_texture(
            "output", avis::rhi::texture_handle{}, avis::rhi::access_states::common, avis::rhi::access_states::common);
        graph.add_graphics_pass<empty_data>(
            "gbuffer",
            [&](render_pass_builder& builder, empty_data&)
            { view_texture = builder.create_texture("albedo", { .size = view_relative(1.0f) }); },
            graphics_execute);
    }

    texture after;
    graph.add_graphics_pass<empty_data>(
        "after",
        [&](render_pass_builder& builder, empty_data&)
        { after = builder.create_texture("late", { .size = absolute(4, 4) }); },
        graphics_execute);

    CHECK(pass_name(graph, 0) == "shared");
    CHECK(pass_name(graph, 1) == "main/gbuffer");
    CHECK(pass_name(graph, 2) == "after");
    CHECK(resource_name(graph, shared_texture) == "instances");
    CHECK(resource_name(graph, view_output) == "main/output");
    CHECK(resource_name(graph, view_texture) == "main/albedo");
    CHECK(resource_name(graph, after) == "late");

    // A new frame may use the name again
    graph.reset();
    {
        const view_scope scope = graph.begin_view("main");
        CHECK(scope.name() == "main");
    }
}

TEST_CASE("render_graph: view scope rules fire their checks", "[render_graph]")
{
    render_graph_fixture fixture;
    render_graph& graph = fixture.graph;
    const avis::tests::recorded_assertions assertions;
    const avis::tests::log_capture capture{ "graphics" };

    SECTION("no nesting")
    {
        const view_scope outer = graph.begin_view("main");
        const view_scope inner = graph.begin_view("topdown");
        CHECK(inner.name().empty());
#ifndef NDEBUG
        CHECK(assertions.contains("no nesting"));
#endif
        // The refused scope qualifies nothing and leaves the outer one open
        graph.add_graphics_pass<empty_data>("pass", no_setup, graphics_execute);
        CHECK(pass_name(graph, 0) == "main/pass");
    }

    SECTION("unique per frame")
    {
        {
            const view_scope first = graph.begin_view("main");
        }
        {
            const view_scope second = graph.begin_view("main");
        }
#ifndef NDEBUG
        CHECK(assertions.contains("already used this frame"));
#endif
    }

    SECTION("names are non-empty and contain no slash")
    {
        {
            const view_scope slash = graph.begin_view("main/left");
            CHECK(slash.name().empty());
        }
        {
            const view_scope empty = graph.begin_view("");
            CHECK(empty.name().empty());
        }
#ifndef NDEBUG
        CHECK(assertions.count() == 2);
        CHECK(assertions.contains("contain no '/'"));
#endif
    }

    SECTION("none open at compile")
    {
        const view_scope scope = graph.begin_view("main");
        graph.compile();
#ifndef NDEBUG
        CHECK(assertions.contains("compile: a view scope is still open"));
#endif
    }
}
