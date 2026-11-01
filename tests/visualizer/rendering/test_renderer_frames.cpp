// Whole renderer frames (docs/tasks/11-renderer.md §11.2): the background, several views, views without output,
// invalid views, the view history store, and the renderer's teardown. On the renderer fixture, with the debug layer and
// GPU-based validation: the RHI capture fails a test on any validation message.

#include "helpers/renderer_fixture.h"
#include "rendering/scene_renderer.h"
#include "rendering/view.h"
#include "rendering/view_history.h"

#include "avis/graphics/error.h"
#include "avis/graphics/graph_debug_information.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/d3d12/resource_manager.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::background_rgba8;
using avis::tests::renderer_fixture;

namespace
{
    const graphics::pass_debug_information* find_pass(const renderer_fixture& fixture, const std::string_view name)
    {
        const renderer_debug_information information = fixture.renderer->debug_information();
        REQUIRE(information.graph != nullptr);
        const auto found = std::ranges::find(information.graph->passes, name, &graphics::pass_debug_information::name);
        return found == information.graph->passes.end() ? nullptr : &*found;
    }
} // namespace

TEST_CASE_METHOD(renderer_fixture, "an empty snapshot with a sun renders the background", "[renderer]")
{
    // Until the sky exists (11.11), the background is background_color
    add_sun({ 1.0f, -1.0f, -1.0f });
    publish();

    const std::vector views{ view() };
    const std::vector images = render_and_read(views);
    REQUIRE(images.size() == 1);
    CHECK(images[0].width == output_size.width);
    CHECK(images[0].height == output_size.height);
    CHECK(images[0].count_different(background_rgba8) == 0);

    const graphics::pass_debug_information* present = find_pass(*this, "main/present_to_shared");
    REQUIRE(present != nullptr);
    CHECK_FALSE(present->culled);

    const renderer_debug_information information = renderer->debug_information();
    CHECK(information.frames_rendered == 1);
    CHECK(information.views == 1);
    CHECK(information.last_publish_sequence == snapshots.publish_sequence());
}

TEST_CASE_METHOD(renderer_fixture, "two views of different sizes render in one frame", "[renderer]")
{
    const rhi::texture_handle small_output = make_output({ 64, 36 });
    const std::vector views{ view("main"), view("top", small_output) };

    const std::vector images = render_and_read(views, 3);
    REQUIRE(images.size() == 2);
    CHECK(images[0].width == 128);
    CHECK(images[0].height == 72);
    CHECK(images[1].width == 64);
    CHECK(images[1].height == 36);
    CHECK(images[0].count_different(background_rgba8) == 0);
    CHECK(images[1].count_different(background_rgba8) == 0);

    CHECK(find_pass(*this, "main/present_to_shared") != nullptr);
    CHECK(find_pass(*this, "top/present_to_shared") != nullptr);
    CHECK(renderer->debug_information().views == 2);

    const view_history* main_history = renderer->view_histories().find("main");
    const view_history* top_history = renderer->view_histories().find("top");
    REQUIRE(main_history != nullptr);
    REQUIRE(top_history != nullptr);
    CHECK(main_history->size == graphics::texture_extent{ 128, 72 });
    CHECK(top_history->size == graphics::texture_extent{ 64, 36 });
}

TEST_CASE_METHOD(renderer_fixture, "a view without an output renders nothing", "[renderer]")
{
    view_description hidden = view("hidden");
    hidden.output.reset();
    const std::vector views{ hidden };

    const result<graphics::submitted_frame> submitted = render_frame(views);
    REQUIRE(submitted.has_value());

    const renderer_debug_information information = renderer->debug_information();
    REQUIRE(information.graph != nullptr);
    CHECK(find_pass(*this, "hidden/present_to_shared") == nullptr);
    for (const graphics::pass_debug_information& pass : information.graph->passes)
    {
        CAPTURE(pass.name);
        CHECK(pass.culled);
    }
}

TEST_CASE_METHOD(renderer_fixture, "invalid views fail the frame and submit nothing", "[renderer]")
{
    std::vector<view_description> views;
    SECTION("two views with one name")
    {
        views = { view("main"), view("main", make_output({ 64, 36 })) };
    }
    SECTION("a name with '/'")
    {
        views = { view("main/left") };
    }
    SECTION("an empty name")
    {
        views = { view("") };
    }
    SECTION("an output of another size")
    {
        view_description wrong = view("main");
        wrong.size = { 64, 36 };
        views = { wrong };
    }
    SECTION("an output of another format")
    {
        auto texture = rhi::resources{ *graphics.hardware_interface }.create_texture(
            { .width = 128,
              .height = 72,
              .format = rhi::format::rgba16_float,
              .usage = rhi::texture_usage::copy_destination,
              .debug_name = "wrong format" });
        REQUIRE(texture.has_value());
        views = { view("main", *texture) };
        const result<graphics::submitted_frame> submitted = render_frame(views);
        REQUIRE_FALSE(submitted.has_value());
        CHECK(graphics::is_error(submitted.error(), graphics::error_kind::invalid_argument));
        rhi::resources{ *graphics.hardware_interface }.destroy(*texture);
        views.clear();
    }

    if (!views.empty())
    {
        const result<graphics::submitted_frame> submitted = render_frame(views);
        REQUIRE_FALSE(submitted.has_value());
        CHECK(graphics::is_error(submitted.error(), graphics::error_kind::invalid_argument));
    }

    // The next frame renders
    const std::vector valid{ view("main") };
    CHECK(render_and_read(valid)[0].count_different(background_rgba8) == 0);
}

TEST_CASE_METHOD(renderer_fixture, "view histories ping-pong, reset, resize and expire", "[renderer]")
{
    std::vector views{ view("main") };
    render_and_read(views);
    const view_history* history = renderer->view_histories().find("main");
    REQUIRE(history != nullptr);
    CHECK_FALSE(history->has_previous);
    const std::uint32_t first = history->current;
    const rhi::texture_handle first_taa = history->taa_history[first];
    CHECK(history->taa_history[0] != history->taa_history[1]);
    CHECK(history->exposure.is_valid());

    SECTION("the slots swap every frame")
    {
        render_and_read(views);
        CHECK(history->has_previous);
        CHECK(history->current == 1 - first);
        CHECK(history->taa_history[history->previous()] == first_taa);
        render_and_read(views);
        CHECK(history->current == first);
    }

    SECTION("reset_history starts again without a previous frame")
    {
        views[0].options.reset_history = true;
        render_and_read(views);
        CHECK_FALSE(history->has_previous);
        CHECK(history->taa_history[first] == first_taa); // the resources stay
    }

    SECTION("a new size recreates the resources")
    {
        const std::vector resized{ view("main", make_output({ 64, 36 })) };
        render_and_read(resized);
        history = renderer->view_histories().find("main");
        REQUIRE(history != nullptr);
        CHECK(history->size == graphics::texture_extent{ 64, 36 });
        CHECK_FALSE(history->has_previous);
        CHECK(std::ranges::find(history->taa_history, first_taa) == history->taa_history.end());
    }

    SECTION("a view unused for 60 frames is released")
    {
        const std::vector both{ view("main"), view("other", make_output({ 64, 36 })) };
        render_and_read(both); // "other" last used in this frame
        CHECK(renderer->view_histories().size() == 2);

        render_and_read(views, static_cast<int>(view_history_release_frames) - 1);
        CHECK(renderer->view_histories().find("other") != nullptr);

        render_and_read(views);
        CHECK(renderer->view_histories().find("other") == nullptr);
        CHECK(renderer->view_histories().find("main") != nullptr);
    }
}

TEST_CASE_METHOD(renderer_fixture, "destroying the renderer after 10 frames leaks nothing", "[renderer]")
{
    rhi::d3d12::device_state& state = graphics.hardware_interface->state();
    const std::vector views{ view("main"), view("top", make_output({ 64, 36 })) };

    const auto run = [&]
    {
        render_and_read(views, 10);
        destroy_renderer(); // waits for the GPU, which also runs the deferred destructions
        CHECK(state.lifetime().pending() == 0);
    };

    run();
    const std::uint32_t textures = state.resources().texture_slots_used();
    const std::uint32_t buffers = state.resources().buffer_slots_used();

    // A second renderer reuses the slots the first one freed: nothing it created is still alive
    create_renderer();
    run();
    CHECK(state.resources().texture_slots_used() == textures);
    CHECK(state.resources().buffer_slots_used() == buffers);

    create_renderer(); // the fixture's teardown expects one
}
