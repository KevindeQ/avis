// The pipeline cache (docs/tasks/08-graphics-shaders-and-pipelines.md §8.6), on the RHI fixture with the shaders of
// tests/shaders/graphics/ and their hand-written bindings

#include "graphics_test_binding_layout_shader.h"
#include "graphics_test_buffers_shader.h"
#include "graphics_test_draw_pixel_shader.h"
#include "graphics_test_draw_vertex_shader.h"
#include "helpers/graphics_test_shaders.h"
#include "helpers/rhi_fixture.h"
#include "helpers/runtime_fixture.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/error.h"
#include "avis/graphics/pipeline_cache.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_library.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/pipeline_factory.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/pipeline.h"
#include "avis/runtime.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace avis;
namespace test_shaders = avis::tests::graphics_test_shaders;

namespace
{
    template<std::size_t Size>
    std::vector<std::byte> bytes(const unsigned char (&code)[Size])
    {
        const auto view = std::as_bytes(std::span{ code });
        return { view.begin(), view.end() };
    }

    graphics::shader_blob
        blob(const std::string_view id, const graphics::shader_stage stage, std::vector<std::byte> code)
    {
        return { .id = std::string{ id },
                 .stage = stage,
                 .bytecode = std::move(code),
                 .bindings = test_shaders::expected_bindings(id) };
    }

    // The test shaders with their hand-written bindings, plus "unbound.main.cs": the binding layout shader without any
    graphics::shader_library make_library(const std::string_view name = "test shaders")
    {
        std::vector<graphics::shader_blob> blobs;
        blobs.push_back(
            blob(test_shaders::draw_vertex, graphics::shader_stage::vertex, bytes(graphics_test_draw_vertex_shader)));
        blobs.push_back(
            blob(test_shaders::draw_pixel, graphics::shader_stage::pixel, bytes(graphics_test_draw_pixel_shader)));
        blobs.push_back(blob(
            test_shaders::binding_layout_compute,
            graphics::shader_stage::compute,
            bytes(graphics_test_binding_layout_shader)));
        blobs.push_back(
            blob(test_shaders::buffers_compute, graphics::shader_stage::compute, bytes(graphics_test_buffers_shader)));
        blobs.push_back(
            { .id = "unbound.main.cs",
              .stage = graphics::shader_stage::compute,
              .bytecode = bytes(graphics_test_binding_layout_shader) });
        auto library = graphics::shader_library::create(std::move(blobs), std::string{ name });
        REQUIRE(library.has_value());
        return std::move(*library);
    }

    // A colour pass into one rgba8 target, without depth
    rhi::graphics_pipeline_state color_state()
    {
        rhi::graphics_pipeline_state state;
        state.depth.test = false;
        state.depth.write = false;
        state.render_target_count = 1;
        state.render_target_formats[0] = rhi::format::rgba8_unorm;
        return state;
    }

    graphics::graphics_pipeline_description draw_description(const std::string_view debug_name = "draw")
    {
        return { .vertex_shader = graphics::make_shader_id(test_shaders::draw_vertex),
                 .pixel_shader = graphics::make_shader_id(test_shaders::draw_pixel),
                 .state = color_state(),
                 .root_signature = std::nullopt,
                 .debug_name = debug_name };
    }

    bool contains(const std::string& text, const std::string_view part)
    {
        return text.find(part) != std::string::npos;
    }

    // The RHI, a registry with the standard and the buffers signature (in that order), a library and a cache
    struct cache_fixture
    {
        cache_fixture() :
            registry{ rhi::pipelines{ *hardware_interface } },
            standard{ registry.add("standard", graphics::binding_layout::standard_description()).value() },
            buffers{ registry.add("buffers", test_shaders::buffers_description()).value() },
            library{ make_library() },
            cache{ rhi::pipelines{ *hardware_interface }, registry }
        {}

        // Pipelines the RHI created so far, graphics and compute
        std::uint32_t pipeline_count()
        {
            rhi::d3d12::pipeline_factory& factory = hardware_interface->state().pipelines();
            return factory.graphics_pipeline_count() + factory.compute_pipeline_count();
        }

        // The root signature a pipeline was created with, through the RHI's records
        template<class Handle>
        bool uses(const Handle pipeline, const rhi::root_signature_handle signature)
        {
            rhi::d3d12::pipeline_factory& factory = hardware_interface->state().pipelines();
            const auto* record = factory.find(pipeline);
            return record != nullptr && record->root_signature == factory.find(signature);
        }

        tests::test_rhi hardware_interface;
        graphics::root_signature_registry registry;
        rhi::root_signature_handle standard;
        rhi::root_signature_handle buffers;
        graphics::shader_library library;
        graphics::pipeline_cache cache;
    };
} // namespace

TEST_CASE_METHOD(cache_fixture, "pipeline_cache returns one handle per distinct description", "[graphics]")
{
    const auto first = cache.get_or_create(library, draw_description());
    REQUIRE(first.has_value());
    CHECK(uses(*first, standard));
    const std::uint32_t created = pipeline_count();

    SECTION("an equal description, or one that differs only in debug_name, gets the same handle")
    {
        CHECK(cache.get_or_create(library, draw_description()) == *first);
        CHECK(cache.get_or_create(library, draw_description("another name")) == *first);
        CHECK(pipeline_count() == created);
        CHECK(cache.size() == 1);
    }

    SECTION("a different state field creates a new one")
    {
        auto description = draw_description();
        description.state.rasterizer.cull = rhi::cull_mode::none;
        const auto culled = cache.get_or_create(library, description);
        REQUIRE(culled.has_value());
        CHECK(*culled != *first);

        description = draw_description();
        description.state.blend[0].write_mask = 0x7;
        const auto masked = cache.get_or_create(library, description);
        REQUIRE(masked.has_value());
        CHECK(*masked != *first);
        CHECK(*masked != *culled);
        CHECK(pipeline_count() == created + 2);
    }

    SECTION("a different explicit root signature creates a new one")
    {
        const auto copy = registry.add("standard copy", graphics::binding_layout::standard_description());
        REQUIRE(copy.has_value());

        auto description = draw_description();
        description.root_signature = standard;
        const auto named = cache.get_or_create(library, description);
        REQUIRE(named.has_value());
        CHECK(*named != *first);
        CHECK(uses(*named, standard));

        description.root_signature = *copy;
        const auto named_copy = cache.get_or_create(library, description);
        REQUIRE(named_copy.has_value());
        CHECK(*named_copy != *named);
        CHECK(uses(*named_copy, *copy));
    }

    SECTION("the same ids from a different library create a new one")
    {
        const graphics::shader_library other = make_library("other shaders");
        const auto from_other = cache.get_or_create(other, draw_description());
        REQUIRE(from_other.has_value());
        CHECK(*from_other != *first);
        CHECK(cache.get_or_create(other, draw_description()) == *from_other);
    }
}

TEST_CASE_METHOD(cache_fixture, "pipeline_cache checks shaders before any RHI call", "[graphics]")
{
    const std::uint32_t created = pipeline_count();
    const std::uint64_t lookups = registry.lookup_count();

    SECTION("an unknown id")
    {
        auto description = draw_description("unknown pixel");
        description.pixel_shader = graphics::make_shader_id("missing.main.ps");
        const auto pipeline = cache.get_or_create(library, description);
        REQUIRE_FALSE(pipeline.has_value());
        CHECK(graphics::is_error(pipeline.error(), graphics::error_kind::shader_not_found));
        CHECK(contains(pipeline.error().message, "pipeline 'unknown pixel': no pixel shader"));
        CHECK(contains(pipeline.error().message, "'test shaders'"));

        const auto compute = cache.get_or_create(
            library,
            graphics::compute_pipeline_description{ .compute_shader = graphics::make_shader_id("missing.cs") });
        REQUIRE_FALSE(compute.has_value());
        CHECK(graphics::is_error(compute.error(), graphics::error_kind::shader_not_found));
    }

    SECTION("a shader in the wrong slot")
    {
        auto description = draw_description("swapped");
        std::swap(description.vertex_shader, description.pixel_shader);
        const auto pipeline = cache.get_or_create(library, description);
        REQUIRE_FALSE(pipeline.has_value());
        CHECK(graphics::is_error(pipeline.error(), graphics::error_kind::shader_stage_mismatch));
        CHECK(
            pipeline.error().message == "pipeline 'swapped': shader 'pipeline_test_draw.pixel_main.ps' is a pixel "
                                        "shader, used as the vertex shader");

        const auto compute = cache.get_or_create(
            library,
            graphics::compute_pipeline_description{ .compute_shader =
                                                        graphics::make_shader_id(test_shaders::draw_vertex),
                                                    .debug_name = "vertex as compute" });
        REQUIRE_FALSE(compute.has_value());
        CHECK(graphics::is_error(compute.error(), graphics::error_kind::shader_stage_mismatch));
        CHECK(contains(compute.error().message, "used as the compute shader"));
    }

    CHECK(pipeline_count() == created);
    CHECK(registry.lookup_count() == lookups);
    CHECK(cache.size() == 0);
}

TEST_CASE_METHOD(cache_fixture, "pipeline_cache picks or checks the root signature", "[graphics]")
{
    SECTION("each shader gets the first compatible signature")
    {
        const auto standard_compute = cache.get_or_create(
            library,
            graphics::compute_pipeline_description{
                .compute_shader = graphics::make_shader_id(test_shaders::binding_layout_compute) });
        REQUIRE(standard_compute.has_value());
        CHECK(uses(*standard_compute, standard));

        const auto buffers_compute = cache.get_or_create(
            library,
            graphics::compute_pipeline_description{ .compute_shader =
                                                        graphics::make_shader_id(test_shaders::buffers_compute) });
        REQUIRE(buffers_compute.has_value());
        CHECK(uses(*buffers_compute, buffers));
    }

    SECTION("a named signature that doesn't fit")
    {
        const auto pipeline = cache.get_or_create(
            library,
            graphics::compute_pipeline_description{ .compute_shader =
                                                        graphics::make_shader_id(test_shaders::buffers_compute),
                                                    .root_signature = standard,
                                                    .debug_name = "buffers on standard" });
        REQUIRE_FALSE(pipeline.has_value());
        CHECK(graphics::is_error(pipeline.error(), graphics::error_kind::root_signature_incompatible));
        CHECK(
            pipeline.error().message ==
            "pipeline 'buffers on standard': root signature 'standard': compute shader t0 space0: no root shader "
            "resource");
    }

    SECTION("a shader without bindings needs a named signature")
    {
        const std::uint32_t created = pipeline_count();
        const graphics::compute_pipeline_description unnamed{ .compute_shader =
                                                                  graphics::make_shader_id("unbound.main.cs"),
                                                              .debug_name = "unbound" };
        const auto pipeline = cache.get_or_create(library, unnamed);
        REQUIRE_FALSE(pipeline.has_value());
        CHECK(graphics::is_error(pipeline.error(), graphics::error_kind::shader_bindings_missing));
        CHECK(contains(pipeline.error().message, "pipeline 'unbound': shader 'unbound.main.cs' has no bindings"));
        CHECK(pipeline_count() == created);

        // Named, it's created without a check: even the buffers signature, which doesn't fit what it really binds
        auto named = unnamed;
        named.root_signature = standard;
        const auto created_pipeline = cache.get_or_create(library, named);
        REQUIRE(created_pipeline.has_value());
        CHECK(uses(*created_pipeline, standard));
    }

    SECTION("matching runs once per key")
    {
        const std::uint64_t before = registry.lookup_count();
        const auto first = cache.get_or_create(library, draw_description());
        REQUIRE(first.has_value());
        CHECK(registry.lookup_count() == before + 1);

        CHECK(cache.get_or_create(library, draw_description()) == *first);
        CHECK(cache.get_or_create(library, draw_description("renamed")) == *first);
        CHECK(registry.lookup_count() == before + 1);
    }
}

TEST_CASE_METHOD(cache_fixture, "pipeline_cache creates depth-only pipelines", "[graphics]")
{
    rhi::graphics_pipeline_state state;
    state.render_target_count = 0;
    state.depth_format = rhi::format::d32_float;

    const graphics::graphics_pipeline_description description{ .vertex_shader =
                                                                   graphics::make_shader_id(test_shaders::draw_vertex),
                                                               .pixel_shader = {},
                                                               .state = state,
                                                               .debug_name = "depth only" };
    const auto pipeline = cache.get_or_create(library, description);
    REQUIRE(pipeline.has_value());
    CHECK(uses(*pipeline, standard));
    CHECK(cache.get_or_create(library, description) == *pipeline);
}

TEST_CASE("pipeline_cache from 8 parallel workers", "[graphics]")
{
    constexpr std::size_t workers = 8;
    constexpr std::size_t descriptions = 50;
    avis::runtime runtime{ tests::small_runtime_configuration(workers) };
    cache_fixture fixture;

    // 50 distinct descriptions: the draw pipeline with 50 depth biases
    std::vector<graphics::graphics_pipeline_description> all;
    for (std::size_t index = 0; index < descriptions; ++index)
    {
        auto description = draw_description("parallel");
        description.state.rasterizer.depth_bias = static_cast<std::int32_t>(index);
        all.push_back(description);
    }
    const std::uint32_t created = fixture.pipeline_count();

    // Each worker asks for all of them in its own order and records the handle it got for each
    std::vector<std::vector<std::optional<rhi::graphics_pipeline_handle>>> handles(
        workers, std::vector<std::optional<rhi::graphics_pipeline_handle>>(descriptions));
    std::mutex mutex;
    int failures = 0;
    auto work = stdexec::schedule(stdexec::get_parallel_scheduler()) |
                stdexec::bulk(
                    stdexec::par,
                    workers,
                    [&](const std::size_t worker)
                    {
                        std::vector<std::size_t> order(descriptions);
                        std::iota(order.begin(), order.end(), std::size_t{ 0 });
                        std::shuffle(order.begin(), order.end(), std::mt19937{ static_cast<std::uint32_t>(worker) });
                        for (const std::size_t index : order)
                        {
                            const auto pipeline = fixture.cache.get_or_create(fixture.library, all[index]);
                            if (pipeline)
                            {
                                handles[worker][index] = *pipeline;
                            }
                            else
                            {
                                const std::lock_guard lock{ mutex };
                                ++failures;
                            }
                        }
                    });
    REQUIRE(stdexec::sync_wait(std::move(work)).has_value());
    REQUIRE(failures == 0);

    for (std::size_t index = 0; index < descriptions; ++index)
    {
        CAPTURE(index);
        REQUIRE(handles[0][index].has_value());
        for (std::size_t worker = 1; worker < workers; ++worker)
        {
            CHECK(handles[worker][index] == handles[0][index]);
        }
        for (std::size_t other = 0; other < index; ++other)
        {
            CHECK(handles[0][other] != handles[0][index]);
        }
    }
    CHECK(fixture.cache.size() == descriptions);

    // Two workers racing on the same description both create its PSO and the cache keeps the first (D-105); the
    // shuffled orders keep that rare, so at most one extra PSO per description
    const std::uint32_t pipelines = fixture.pipeline_count() - created;
    INFO(std::format("{} PSOs for {} descriptions", pipelines, descriptions));
    CHECK(pipelines >= descriptions);
    CHECK(pipelines <= descriptions * 2);
}
