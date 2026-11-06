// Screenshots (docs/tasks/11-renderer.md §11.21): the PNG helpers and the request checks as unit tests, then viewport
// and custom screenshots on the renderer fixture, with the debug layer and GPU-based validation.

#include "helpers/renderer_fixture.h"
#include "helpers/temporary_directory.h"
#include "rendering/screenshot_capture.h"
#include "rendering/view.h"

#include "avis/io/file_operations.h"
#include "avis/scene/assets/material.h"

#include <Eigen/Core>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::renderer_fixture;
using avis::tests::rgba8;
using avis::tests::rgba8_image;
using avis::tests::temporary_directory;

namespace
{
    std::vector<std::byte> read_bytes(const std::filesystem::path& path)
    {
        std::ifstream file{ path, std::ios::binary };
        REQUIRE(file.good());
        const std::vector<char> characters{ std::istreambuf_iterator<char>{ file }, {} };
        std::vector<std::byte> bytes(characters.size());
        std::ranges::transform(characters, bytes.begin(), [](const char c) { return static_cast<std::byte>(c); });
        return bytes;
    }

    std::optional<std::string> text_of(const std::vector<png_text>& text, const std::string_view keyword)
    {
        const auto found = std::ranges::find(text, keyword, &png_text::keyword);
        return found != text.end() ? std::optional{ found->text } : std::nullopt;
    }

    // Takes the renderer's screenshot results until `count` have arrived; the files are written on parallel workers
    std::vector<screenshot_result> wait_for_screenshots(renderer_fixture& fixture, const std::size_t count)
    {
        std::vector<screenshot_result> results;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 30 };
        while (results.size() < count && std::chrono::steady_clock::now() < deadline)
        {
            std::vector<screenshot_result> taken = fixture.renderer->take_screenshot_results();
            results.insert(results.end(), taken.begin(), taken.end());
            if (results.size() < count)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 2 });
            }
        }
        REQUIRE(results.size() == count);
        return results;
    }

    // A view without TAA or sky: the image of a frame doesn't depend on the frames before it
    view_description steady_view(renderer_fixture& fixture)
    {
        fixture.renderer->set_environment_override(Eigen::Vector3f::Zero());
        view_description made = fixture.view();
        made.options.taa = false;
        return made;
    }

    scene::material_description unlit_white()
    {
        scene::material_description material;
        material.shading = scene::shading_model::unlit;
        material.base_color_factor = { 1.0f, 1.0f, 1.0f, 1.0f };
        return material;
    }

    std::uint8_t green_at(const png_image& image, const std::uint32_t x, const std::uint32_t y)
    {
        return image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4 + 1];
    }

    struct silhouette
    {
        std::uint32_t left = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t top = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t right = 0; // inclusive
        std::uint32_t bottom = 0;
        std::size_t partial = 0; // pixels more than 3 levels from both the background and the inside
    };

    // The pixels at least halfway from `background` to `inside` (the green channel), and the partly covered ones: the
    // tonemap and the sRGB curve bring a pixel 60% covered to about 90% of the inside's level, so any level between
    // the two counts (the dither moves a level by at most 1)
    silhouette measure(const png_image& image, const std::uint8_t background, const std::uint8_t inside)
    {
        silhouette found;
        for (std::uint32_t y = 0; y < image.height; ++y)
        {
            for (std::uint32_t x = 0; x < image.width; ++x)
            {
                const double fraction = (static_cast<double>(green_at(image, x, y)) - background) /
                                        (static_cast<double>(inside) - background);
                if (fraction >= 0.5)
                {
                    found.left = std::min(found.left, x);
                    found.top = std::min(found.top, y);
                    found.right = std::max(found.right, x);
                    found.bottom = std::max(found.bottom, y);
                }
                const int level = green_at(image, x, y);
                if (std::abs(level - int{ background }) > 3 && std::abs(level - int{ inside }) > 3)
                {
                    ++found.partial;
                }
            }
        }
        return found;
    }

    png_image image_of(const rgba8_image& read)
    {
        png_image image{ .width = read.width, .height = read.height };
        for (const rgba8 pixel : read.pixels)
        {
            image.rgba.insert(image.rgba.end(), pixel.begin(), pixel.end());
        }
        return image;
    }
} // namespace

// ---- CPU ----

TEST_CASE("a screenshot PNG holds the pixels with alpha 255 and the text chunks", "[renderer][unit]")
{
    // 3 × 2 RGBA pixels in rows 16 bytes apart, alphas that the PNG doesn't keep
    std::vector<std::byte> pixels(16 * 2);
    for (std::uint32_t y = 0; y < 2; ++y)
    {
        for (std::uint32_t x = 0; x < 3; ++x)
        {
            const std::size_t at = y * 16 + x * 4;
            pixels[at + 0] = static_cast<std::byte>(10 * x + 1);
            pixels[at + 1] = static_cast<std::byte>(100 + y);
            pixels[at + 2] = static_cast<std::byte>(200 + x + y);
            pixels[at + 3] = static_cast<std::byte>(7);
        }
    }
    const std::vector<png_text> text{ { "Scenario", "drive" }, { "Render settings", "taa: true\ntonemapper: aces" } };
    const auto png = encode_png(3, 2, pixels, 16, text);
    REQUIRE(png.has_value());

    const auto decoded = decode_png(*png);
    REQUIRE(decoded.has_value());
    CHECK(decoded->width == 3);
    CHECK(decoded->height == 2);
    for (std::uint32_t y = 0; y < 2; ++y)
    {
        for (std::uint32_t x = 0; x < 3; ++x)
        {
            const std::size_t at = (y * 3 + x) * 4;
            CHECK(decoded->rgba[at + 0] == 10 * x + 1);
            CHECK(decoded->rgba[at + 1] == 100 + y);
            CHECK(decoded->rgba[at + 2] == 200 + x + y);
            CHECK(decoded->rgba[at + 3] == 255);
        }
    }

    const std::vector<png_text> read = read_png_text(*png);
    REQUIRE(read.size() == 2);
    CHECK(read[0].keyword == "Scenario");
    CHECK(read[0].text == "drive");
    CHECK(read[1].keyword == "Render settings");
    CHECK(read[1].text == "taa: true\ntonemapper: aces");

    CHECK(read_png_text(std::vector<std::byte>(16)).empty()); // not a PNG
    CHECK_FALSE(encode_png(3, 2, pixels, 16, std::vector<png_text>{ { " leading", "x" } }).has_value());
    CHECK_FALSE(encode_png(3, 2, pixels, 16, std::vector<png_text>{ { "", "x" } }).has_value());
    CHECK_FALSE(encode_png(3, 3, pixels, 16, {}).has_value()); // a row short
    CHECK_FALSE(encode_png(0, 2, pixels, 16, {}).has_value());
}

TEST_CASE("a screenshot's metadata becomes text chunks", "[renderer][unit]")
{
    const std::vector<png_text> text = screenshot_text(
        { .scenario = "drive",
          .step = 42,
          .simulation_time = 1.5,
          .driver_id = "driver-7",
          .camera_pose = "position 1 2 3" });
    CHECK(text_of(text, "Software") == "Avis");
    CHECK(text_of(text, "Scenario") == "drive");
    CHECK(text_of(text, "Step") == "42");
    CHECK(text_of(text, "Simulation time") == "1.5 s");
    CHECK(text_of(text, "Driver") == "driver-7");
    CHECK(text_of(text, "Camera") == "position 1 2 3");
    CHECK_FALSE(text_of(text, "Render settings").has_value()); // empty: left out
}

TEST_CASE("screenshot requests are refused past 8K, without a path or over half the free memory", "[renderer][unit]")
{
    screenshot_request request{ .mode = screenshot_mode::custom, .width = 7680, .height = 4320, .path = "a.png" };
    CHECK_FALSE(screenshot_refusal(request, std::nullopt).has_value());
    request.width = 7681;
    CHECK(screenshot_refusal(request, std::nullopt).has_value());
    request.width = 7680;
    request.height = 4321;
    CHECK(screenshot_refusal(request, std::nullopt).has_value());
    request.height = 0;
    CHECK(screenshot_refusal(request, std::nullopt).has_value());

    request.width = 1920;
    request.height = 1080;
    const std::uint64_t estimate = screenshot_transient_bytes(1920, 1080);
    CHECK_FALSE(screenshot_refusal(request, rhi::memory_budget{ .budget = 2 * estimate + 1000, .usage = 1000 }));
    CHECK(screenshot_refusal(request, rhi::memory_budget{ .budget = 2 * estimate + 1000, .usage = 2000 }));
    CHECK(screenshot_refusal(request, rhi::memory_budget{ .budget = 0, .usage = 1000 })); // over budget already

    screenshot_request viewport{ .mode = screenshot_mode::viewport, .path = "b.png" };
    CHECK_FALSE(screenshot_refusal(viewport, rhi::memory_budget{}).has_value()); // the view's size: no checks
    viewport.path.clear();
    CHECK(screenshot_refusal(viewport, std::nullopt).has_value());
}

// ---- GPU ----

TEST_CASE_METHOD(
    renderer_fixture, "a viewport screenshot's pixels equal the output's, with the metadata as text", "[renderer]")
{
    add_unit_cube({ 0.0f, 0.0f, -5.0f });
    add_sun({ 0.3f, -1.0f, -0.5f });
    publish();
    const temporary_directory directory;
    const std::filesystem::path path = directory.file("viewport.png");

    renderer->request_screenshot(
        { .id = 7,
          .path = path,
          .metadata = { .scenario = "drive",
                        .step = 42,
                        .simulation_time = 1.5,
                        .driver_id = "driver-7",
                        .render_settings = "taa: true" } });
    const std::vector views{ view() };
    const std::vector images = render_and_read(views);
    const std::vector<screenshot_result> results = wait_for_screenshots(*this, 1);
    CHECK(results[0].id == 7);
    CHECK(results[0].path == path);
    REQUIRE_FALSE(results[0].failure.has_value());
    CHECK(results[0].size == output_size);
    CHECK(results[0].first_frame_index == results[0].last_frame_index);

    const std::vector<std::byte> png = read_bytes(path);
    const auto decoded = decode_png(png);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->width == output_size.width);
    REQUIRE(decoded->height == output_size.height);
    std::size_t different = 0;
    for (std::size_t pixel = 0; pixel < images[0].pixels.size(); ++pixel)
    {
        const rgba8 expected = images[0].pixels[pixel];
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            different += decoded->rgba[pixel * 4 + channel] != expected[channel] ? 1 : 0;
        }
        different += decoded->rgba[pixel * 4 + 3] != 255 ? 1 : 0;
    }
    CHECK(different == 0);

    const std::vector<png_text> text = read_png_text(png);
    CHECK(text_of(text, "Scenario") == "drive");
    CHECK(text_of(text, "Step") == "42");
    CHECK(text_of(text, "Simulation time") == "1.5 s");
    CHECK(text_of(text, "Driver") == "driver-7");
    CHECK(text_of(text, "Render settings") == "taa: true");
    const std::optional<std::string> camera = text_of(text, "Camera"); // the view's, since the request gave none
    REQUIRE(camera.has_value());
    CHECK(camera->starts_with("position 0 0 0;"));

    // Nothing else in the folder: the atomic replace's temporary file is gone
    CHECK(std::distance(std::filesystem::directory_iterator{ directory.path() }, {}) == 1);
}

TEST_CASE_METHOD(renderer_fixture, "a viewport screenshot shows the debug blits only when asked to", "[renderer]")
{
    add_unit_cube({ 0.0f, 0.0f, -5.0f });
    publish();
    const temporary_directory directory;
    view_description main = steady_view(*this);
    main.options.debug_blits.push_back({ .resource = "gbuffer_normal", .mode = debug_blit_mode::normal_octahedral });
    const std::vector views{ main };

    renderer->request_screenshot({ .id = 1, .path = directory.file("without.png") });
    const rgba8_image with_blit = render_and_read(views)[0];
    const std::vector<screenshot_result> first = wait_for_screenshots(*this, 1);
    REQUIRE_FALSE(first[0].failure.has_value());

    renderer->request_screenshot({ .id = 2, .include_debug_blit = true, .path = directory.file("with.png") });
    render_and_read(views);
    const std::vector<screenshot_result> second = wait_for_screenshots(*this, 1);
    REQUIRE_FALSE(second[0].failure.has_value());

    const auto without = decode_png(read_bytes(directory.file("without.png")));
    const auto with = decode_png(read_bytes(directory.file("with.png")));
    REQUIRE(without.has_value());
    REQUIRE(with.has_value());
    CHECK(with->rgba == image_of(with_blit).rgba);
    CHECK(without->rgba != with->rgba);
    // The background shows the normal blit's colour of nothing drawn with it, the black sky without it
    CHECK(green_at(*without, 0, 0) == 0);
}

TEST_CASE_METHOD(
    renderer_fixture,
    "a custom screenshot renders the cube at its size, anti-aliased, in exactly 8 frames",
    "[renderer]")
{
    add_unit_cube({ 0.0f, 0.0f, -5.0f }, unlit_white());
    publish();
    // Unlit colours are pre-exposed like radiance: an exposure near 1 shows white as bright grey
    renderer->set_exposure_parameters({ .mode = exposure_mode::manual, .ev100 = 0.0f });
    const temporary_directory directory;
    const std::vector views{ steady_view(*this) };

    renderer->request_screenshot(
        { .id = 3,
          .mode = screenshot_mode::custom,
          .width = 512,
          .height = 288,
          .path = directory.file("custom.png") });
    render_and_read(views, 7);
    CHECK(renderer->take_screenshot_results().empty());
    const renderer_debug_information information = renderer->debug_information();
    CHECK(information.views == 2); // the screenshot view renders next to the main one
    render_and_read(views, 1);
    const std::vector<screenshot_result> results = wait_for_screenshots(*this, 1);
    REQUIRE_FALSE(results[0].failure.has_value());
    CHECK(results[0].size == graphics::texture_extent{ 512, 288 });
    CHECK(results[0].last_frame_index - results[0].first_frame_index + 1 == 8);

    // Once it's done, the frames have only the main view again
    render_and_read(views, 1);
    CHECK(renderer->debug_information().views == 1);

    const auto shot = decode_png(read_bytes(directory.file("custom.png")));
    REQUIRE(shot.has_value());
    REQUIRE(shot->width == 512);
    REQUIRE(shot->height == 288);

    // The same camera without anti-aliasing, at the same size
    const rhi::texture_handle plain_output = make_output({ 512, 288 });
    view_description plain = steady_view(*this);
    plain.size = { 512, 288 };
    plain.output->texture = plain_output;
    const png_image reference = image_of(render_and_read(std::vector{ plain })[0]);

    // The inside and the background match the plain render's
    const std::uint8_t inside = green_at(reference, 256, 144);
    const std::uint8_t background = green_at(reference, 0, 0);
    REQUIRE(inside > background + 100);
    CHECK(std::abs(int{ green_at(*shot, 256, 144) } - int{ inside }) <= 2);
    CHECK(std::abs(int{ green_at(*shot, 0, 0) } - int{ background }) <= 2);

    // The front face, 1 m at 4.5 m with a 60° vertical field of view: 288 · (1 / 4.5) / (2 tan 30°) = 55.4 px square,
    // centred, so from 228.3 to 283.7 across and 116.3 to 171.7 down
    const silhouette measured = measure(*shot, background, inside);
    CHECK(measured.left == 228);
    CHECK(measured.right == 283);
    CHECK(measured.top == 116);
    CHECK(measured.bottom == 171);
    const silhouette hard = measure(reference, background, inside);
    CHECK(hard.left == measured.left);
    CHECK(hard.right == measured.right);
    CHECK(hard.top == measured.top);
    CHECK(hard.bottom == measured.bottom);

    // Its edges are partly covered pixels, where the plain render's are hard: a row or column of them on each side,
    // 55 or 56 pixels long
    CHECK(measured.partial >= 4 * 50);
    CHECK(hard.partial <= 8);
}

TEST_CASE_METHOD(renderer_fixture, "screenshot requests: refused past 8K, queued one at a time", "[renderer]")
{
    add_unit_cube({ 0.0f, 0.0f, -5.0f });
    publish();
    const temporary_directory directory;
    const std::vector views{ steady_view(*this) };

    SECTION("8K + 1 is refused at once")
    {
        renderer->request_screenshot(
            { .id = 1,
              .mode = screenshot_mode::custom,
              .width = 7681,
              .height = 4320,
              .path = directory.file("big.png") });
        const std::vector<screenshot_result> results = renderer->take_screenshot_results();
        REQUIRE(results.size() == 1);
        CHECK(results[0].id == 1);
        REQUIRE(results[0].failure.has_value());
        CHECK(results[0].failure->message.find("7680 x 4320") != std::string::npos);
        render_and_read(views);
        CHECK(renderer->debug_information().views == 1);
        CHECK(std::filesystem::is_empty(directory.path()));
    }

    SECTION("over half the free video memory is refused at once")
    {
        memory_budget_override = rhi::memory_budget{ .budget = 64ull << 20, .usage = 0 };
        renderer->request_screenshot(
            { .id = 2,
              .mode = screenshot_mode::custom,
              .width = 1920,
              .height = 1080,
              .path = directory.file("big.png") });
        const std::vector<screenshot_result> results = renderer->take_screenshot_results();
        REQUIRE(results.size() == 1);
        REQUIRE(results[0].failure.has_value());
        CHECK(results[0].failure->message.find("video memory") != std::string::npos);
    }

    SECTION("a second request during the first is queued and completes after it")
    {
        renderer->request_screenshot(
            { .id = 1,
              .mode = screenshot_mode::custom,
              .width = 64,
              .height = 36,
              .path = directory.file("first.png") });
        render_and_read(views, 2);
        renderer->request_screenshot({ .id = 2, .path = directory.file("second.png") });

        std::vector<screenshot_result> results;
        for (int frame = 0; frame < 40 && results.size() < 2; ++frame)
        {
            render_and_read(views);
            std::vector<screenshot_result> taken = renderer->take_screenshot_results();
            results.insert(results.end(), taken.begin(), taken.end());
            if (results.size() == 1 && frame < 39)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 5 }); // the first one's write
            }
        }
        REQUIRE(results.size() == 2);
        CHECK(results[0].id == 1);
        CHECK(results[1].id == 2);
        CHECK_FALSE(results[0].failure.has_value());
        CHECK_FALSE(results[1].failure.has_value());
        CHECK(results[0].last_frame_index - results[0].first_frame_index + 1 == 8);
        CHECK(results[1].first_frame_index > results[0].last_frame_index);
        CHECK(std::filesystem::exists(directory.file("first.png")));
        CHECK(std::filesystem::exists(directory.file("second.png")));
    }

    SECTION("a queued request is replaced by a later one")
    {
        renderer->request_screenshot({ .id = 1, .path = directory.file("first.png") });
        render_and_read(views);
        renderer->request_screenshot({ .id = 2, .path = directory.file("second.png") });
        renderer->request_screenshot({ .id = 3, .path = directory.file("third.png") });
        std::vector<screenshot_result> results = renderer->take_screenshot_results();
        const auto superseded = std::ranges::find(results, 2u, &screenshot_result::id);
        REQUIRE(superseded != results.end());
        REQUIRE(superseded->failure.has_value());
        CHECK(superseded->failure->message.find("superseded") != std::string::npos);

        std::vector<screenshot_result> written;
        for (int frame = 0; frame < 40 && written.size() < 2; ++frame)
        {
            std::vector<screenshot_result> taken = renderer->take_screenshot_results();
            std::ranges::copy_if(
                taken, std::back_inserter(written), [](const screenshot_result& result) { return result.id != 2; });
            std::ranges::copy_if(
                results, std::back_inserter(written), [](const screenshot_result& result) { return result.id != 2; });
            results.clear();
            render_and_read(views);
            std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
        }
        REQUIRE(written.size() == 2);
        CHECK(written[0].id == 1);
        CHECK(written[1].id == 3);
        CHECK_FALSE(std::filesystem::exists(directory.file("second.png")));
    }
}

TEST_CASE_METHOD(renderer_fixture, "a screenshot whose write fails reports an error and leaves no file", "[renderer]")
{
    publish();
    const temporary_directory directory;
    const std::vector views{ view() };

    SECTION("a folder that doesn't exist")
    {
        renderer->request_screenshot({ .id = 1, .path = directory.file("missing") / "shot.png" });
        render_and_read(views);
        const std::vector<screenshot_result> results = wait_for_screenshots(*this, 1);
        CHECK(results[0].failure.has_value());
        CHECK(std::filesystem::is_empty(directory.path()));
    }

    SECTION("a folder in the way of the file")
    {
        std::filesystem::create_directory(directory.file("shot.png"));
        renderer->request_screenshot({ .id = 1, .path = directory.file("shot.png") });
        render_and_read(views);
        const std::vector<screenshot_result> results = wait_for_screenshots(*this, 1);
        CHECK(results[0].failure.has_value());
        CHECK(std::distance(std::filesystem::directory_iterator{ directory.path() }, {}) == 1); // no temporary file
        CHECK(std::filesystem::is_directory(directory.file("shot.png")));
    }

    SECTION("a write that fails before the rename")
    {
        io::detail::inject_atomic_replace_failure(true);
        renderer->request_screenshot({ .id = 1, .path = directory.file("shot.png") });
        render_and_read(views);
        const std::vector<screenshot_result> results = wait_for_screenshots(*this, 1);
        io::detail::inject_atomic_replace_failure(false);
        CHECK(results[0].failure.has_value());
        CHECK(std::filesystem::is_empty(directory.path()));
    }
}
