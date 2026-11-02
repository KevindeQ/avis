// Sort keys (docs/tasks/11-renderer.md §11.6, 11 §4.2, §4.3): the field layout round trip, the debug checks, the
// ordering the passes rely on, and the per-group depth fields.

#include "helpers/recorded_assertions.h"
#include "rendering/sort_key.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <variant>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;

namespace
{
    constexpr key_group camera_world(const translucency value = translucency::opaque)
    {
        return { fullscreen_layer::scene, viewport::camera(), viewport_layer::world, value };
    }

    constexpr std::uint64_t draw(
        const key_group& group,
        const std::uint32_t depth,
        const std::uint32_t pipeline_set = 0,
        const std::uint32_t material = 0)
    {
        return make_draw_key({ group, depth, pipeline_set, material });
    }

    key_group random_group(std::mt19937_64& random)
    {
        return { static_cast<fullscreen_layer>(random() % 4),
                 viewport{ static_cast<std::uint8_t>(random() % 8) },
                 static_cast<viewport_layer>(random() % 8),
                 static_cast<translucency>(random() % 4) };
    }

    void test_callback(command_context&, const void*) {}

    // Constant expressions: construction and decoding are constexpr
    static_assert(
        std::get<draw_key_fields>(decode_key(draw(camera_world(), 5, 7, 9))) ==
        draw_key_fields{ camera_world(), 5, 7, 9 });
    static_assert(make_draw_key({}) == (std::uint64_t{ 1 } << 56)); // the world viewport layer is 1
    static_assert(viewport::shadow_cascade(3).index == 4 && viewport::shadow_cascade(0).cascade() == 0);
    static_assert(is_command_key(make_command_key({ camera_world(), 0, builtin_command{} })));
} // namespace

TEST_CASE("decode_key inverts make_draw_key and make_command_key", "[renderer][unit]")
{
    std::mt19937_64 random{ 1106 };
    for (int i = 0; i < 1000; ++i)
    {
        const draw_key_fields draw_fields{ random_group(random),
                                           static_cast<std::uint32_t>(random() % (1u << 24)),
                                           static_cast<std::uint32_t>(random() % 4096),
                                           static_cast<std::uint32_t>(random() % 131072) };
        const std::uint64_t draw_key = make_draw_key(draw_fields);
        CHECK_FALSE(is_command_key(draw_key));
        CHECK(group_of(draw_key) == draw_fields.group);
        const decoded_key decoded_draw = decode_key(draw_key);
        REQUIRE(std::holds_alternative<draw_key_fields>(decoded_draw));
        CHECK(std::get<draw_key_fields>(decoded_draw) == draw_fields);

        const command_key_fields command_fields{ random_group(random),
                                                 static_cast<std::uint32_t>(random() % 32),
                                                 builtin_command{ static_cast<command_opcode>(random() % 256),
                                                                  random() % (std::uint64_t{ 1 } << 39) } };
        const std::uint64_t command_key = make_command_key(command_fields);
        CHECK(is_command_key(command_key));
        CHECK(group_of(command_key) == command_fields.group);
        const decoded_key decoded_command = decode_key(command_key);
        REQUIRE(std::holds_alternative<command_key_fields>(decoded_command));
        CHECK(std::get<command_key_fields>(decoded_command) == command_fields);
    }
}

TEST_CASE("a callback command keeps its function address", "[renderer][unit]")
{
    const callback_command callback = callback_command::of(&test_callback);
    CHECK(callback.function() == &test_callback);

    const command_key_fields fields{ camera_world(), 3, callback };
    const std::uint64_t key = make_command_key(fields);
    CHECK(key_fields::callback.extract(key) == 1);
    const auto decoded = std::get<command_key_fields>(decode_key(key));
    CHECK(decoded == fields);
    REQUIRE(std::holds_alternative<callback_command>(decoded.command));
    CHECK(std::get<callback_command>(decoded.command).function() == &test_callback);
}

TEST_CASE("out-of-range sort key fields are debug checks", "[renderer][unit]")
{
    tests::recorded_assertions assertions;

    SECTION("draw fields are truncated to their width")
    {
        const std::uint64_t key = make_draw_key({ camera_world(), 1u << 24, 4096, 131072 + 5 });
        // Truncated, so no field spills into its neighbour
        CHECK(std::get<draw_key_fields>(decode_key(key)) == draw_key_fields{ camera_world(), 0, 0, 5 });
#ifndef NDEBUG
        CHECK(assertions.count() == 3);
        CHECK(assertions.contains("depth"));
        CHECK(assertions.contains("pipeline_set"));
        CHECK(assertions.contains("material"));
#endif
    }

    SECTION("group fields")
    {
        const key_group group{ static_cast<fullscreen_layer>(4),
                               viewport{ 8 },
                               static_cast<viewport_layer>(8),
                               static_cast<translucency>(4) };
        static_cast<void>(make_draw_key({ group, 0, 0, 0 }));
#ifndef NDEBUG
        CHECK(assertions.count() == 4);
        CHECK(assertions.contains("fullscreen_layer"));
        CHECK(assertions.contains("viewport_layer"));
        CHECK(assertions.contains("translucency"));
#endif
        static_cast<void>(viewport::shadow_cascade(maximum_shadow_cascades));
#ifndef NDEBUG
        CHECK(assertions.count() == 5);
        CHECK(assertions.contains("shadow_cascade"));
#endif
    }

    SECTION("command fields")
    {
        static_cast<void>(make_command_key({ camera_world(), 32, builtin_command{ command_opcode::reset_state, 0 } }));
        static_cast<void>(make_command_key(
            { camera_world(), 0, builtin_command{ command_opcode::set_write_mask, std::uint64_t{ 1 } << 39 } }));
        static_cast<void>(make_command_key({ camera_world(), 0, callback_command{ std::uint64_t{ 1 } << 47 } }));
#ifndef NDEBUG
        CHECK(assertions.count() == 3);
        CHECK(assertions.contains("sequence"));
        CHECK(assertions.contains("immediate"));
        CHECK(assertions.contains("callback address"));
#endif
    }

    SECTION("depth fields")
    {
        static_cast<void>(opaque_depth_field(0, 3));
        static_cast<void>(translucent_depth_field(1u << 24));
        static_cast<void>(fullscreen_effects_depth_field(1u << 24));
#ifndef NDEBUG
        CHECK(assertions.count() == 3);
        CHECK(assertions.contains("opaque depth bands"));
        CHECK(assertions.contains("stacking index"));
#endif
    }

    SECTION("valid fields pass")
    {
        static_cast<void>(make_draw_key({ camera_world(translucency::subtractive), maximum_depth, 4095, 131071 }));
        static_cast<void>(make_command_key(
            { key_group{ fullscreen_layer::hud, viewport{ 7 }, viewport_layer::overlay_on_top, translucency::opaque },
              31,
              builtin_command{ command_opcode::reset_state, (std::uint64_t{ 1 } << 39) - 1 } }));
        static_cast<void>(callback_command::of(&test_callback));
        CHECK(assertions.count() == 0);
    }
}

TEST_CASE("a command sorts after the draws of its group and before the next group", "[renderer][unit]")
{
    const std::uint64_t last_draw = make_draw_key({ camera_world(), maximum_depth, 4095, 131071 });
    const std::uint64_t first_command = make_command_key({ camera_world(), 0, builtin_command{} });
    const std::uint64_t last_command =
        make_command_key({ camera_world(), 31, callback_command{ (std::uint64_t{ 1 } << 47) - 1 } });
    const std::uint64_t next_group = make_draw_key({ camera_world(translucency::blended), 0, 0, 0 });
    CHECK(last_draw < first_command);
    CHECK(first_command < last_command);
    CHECK(last_command < next_group);

    // Commands of one group run in sequence order, whatever their command data
    CHECK(
        make_command_key({ camera_world(), 1, builtin_command{ command_opcode::clear_depth_stencil, 0 } }) <
        make_command_key({ camera_world(), 2, builtin_command{ command_opcode::clear_depth_stencil, 0 } }));
    CHECK(
        make_command_key({ camera_world(), 1, callback_command{ (std::uint64_t{ 1 } << 47) - 1 } }) <
        make_command_key({ camera_world(), 2, builtin_command{ command_opcode::clear_depth_stencil, 0 } }));
}

TEST_CASE("groups sort in the order the passes draw them", "[renderer][unit]")
{
    // Translucency: opaque < blended < additive < subtractive, even at extreme depth and material fields
    CHECK(
        draw(camera_world(translucency::opaque), maximum_depth, 4095, 131071) <
        draw(camera_world(translucency::blended), 0));
    CHECK(
        draw(camera_world(translucency::blended), maximum_depth, 4095, 131071) <
        draw(camera_world(translucency::additive), 0));
    CHECK(
        draw(camera_world(translucency::additive), maximum_depth, 4095, 131071) <
        draw(camera_world(translucency::subtractive), 0));

    // overlay before overlay_on_top, and both after world
    const key_group overlay{
        fullscreen_layer::scene, viewport::camera(), viewport_layer::overlay, translucency::subtractive
    };
    const key_group on_top{
        fullscreen_layer::scene, viewport::camera(), viewport_layer::overlay_on_top, translucency::opaque
    };
    CHECK(draw(camera_world(translucency::subtractive), maximum_depth) < draw(overlay, 0));
    CHECK(draw(overlay, maximum_depth, 4095, 131071) < draw(on_top, 0));

    // The camera before the shadow cascades, the cascades in order, and the scene before fullscreen effects
    const key_group cascade_0{
        fullscreen_layer::scene, viewport::shadow_cascade(0), viewport_layer::world, translucency::opaque
    };
    const key_group cascade_3{
        fullscreen_layer::scene, viewport::shadow_cascade(3), viewport_layer::world, translucency::opaque
    };
    const key_group effects{
        fullscreen_layer::fullscreen_effects, viewport::camera(), viewport_layer::background, translucency::blended
    };
    CHECK(draw(on_top, maximum_depth) < draw(cascade_0, 0));
    CHECK(draw(cascade_0, maximum_depth) < draw(cascade_3, 0));
    CHECK(draw(cascade_3, maximum_depth) < draw(effects, 0));
}

TEST_CASE("opaque items of one band sort by pipeline set, then material", "[renderer][unit]")
{
    const std::uint32_t band = opaque_depth_field(quantize_depth(1.0f), 16);
    REQUIRE(band == opaque_depth_field(quantize_depth(1.1f), 16));

    // Inserted shuffled; sorted by (pipeline set, material) since the depth field is equal
    std::vector<std::uint64_t> keys{
        draw(camera_world(), band, 2, 1), draw(camera_world(), band, 0, 9), draw(camera_world(), band, 1, 0),
        draw(camera_world(), band, 0, 3), draw(camera_world(), band, 2, 0),
    };
    std::ranges::sort(keys);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order;
    for (const std::uint64_t key : keys)
    {
        const auto fields = std::get<draw_key_fields>(decode_key(key));
        order.emplace_back(fields.pipeline_set, fields.material);
    }
    CHECK(
        order ==
        std::vector<std::pair<std::uint32_t, std::uint32_t>>{ { 0, 3 }, { 0, 9 }, { 1, 0 }, { 2, 0 }, { 2, 1 } });

    // A nearer band sorts first whatever its pipeline set
    const std::uint32_t far_band = opaque_depth_field(quantize_depth(100.0f), 16);
    CHECK(band < far_band);
    CHECK(draw(camera_world(), band, 4095, 131071) < draw(camera_world(), far_band, 0, 0));
}

TEST_CASE("translucent items sort far to near", "[renderer][unit]")
{
    const float distances[] = { 0.5f, 3.0f, 3.1f, 40.0f, 900.0f };
    std::vector<std::uint64_t> keys;
    for (const float distance : distances)
    {
        // The material fields are in reverse so they can't give the order
        keys.push_back(draw(
            camera_world(translucency::blended),
            translucent_depth_field(quantize_depth(distance)),
            static_cast<std::uint32_t>(keys.size()),
            static_cast<std::uint32_t>(keys.size())));
    }
    std::vector<std::uint64_t> sorted = keys;
    std::ranges::sort(sorted);
    CHECK(std::ranges::equal(sorted, std::vector<std::uint64_t>(keys.rbegin(), keys.rend())));
}

TEST_CASE("quantize_depth is logarithmic over [0.1 m, 2 km]", "[renderer][unit]")
{
    CHECK(quantize_depth(depth_near) == 0);
    CHECK(quantize_depth(depth_far) == maximum_depth);
    CHECK(quantize_depth(0.0f) == 0);
    CHECK(quantize_depth(-5.0f) == 0);
    CHECK(quantize_depth(1.0e6f) == maximum_depth);

    // Logarithmic: equal ratios give equal steps, and the geometric middle of the range is half way
    const double middle = std::sqrt(double{ depth_near } * double{ depth_far });
    CHECK(
        quantize_depth(static_cast<float>(middle)) / static_cast<double>(maximum_depth) ==
        Catch::Approx(0.5).margin(1.0e-6));

    // Monotonic
    std::uint32_t previous = 0;
    for (float distance = 0.05f; distance < 2500.0f; distance *= 1.01f)
    {
        const std::uint32_t q = quantize_depth(distance);
        CHECK(q >= previous);
        previous = q;
    }

    // With 16 bands, 1 m and 1.1 m share a band; 1 m and 10 m don't
    CHECK(opaque_depth_field(quantize_depth(1.0f), 16) == opaque_depth_field(quantize_depth(1.1f), 16));
    CHECK(opaque_depth_field(quantize_depth(1.0f), 16) != opaque_depth_field(quantize_depth(10.0f), 16));
}

TEST_CASE("the per-group depth fields", "[renderer][unit]")
{
    const std::uint32_t q = quantize_depth(25.0f);

    // Opaque: the band in the top log2(bands) bits
    CHECK(opaque_depth_field(q, 16) == (q & 0xF00000u));
    CHECK(opaque_depth_field(q, 1) == 0);
    CHECK(opaque_depth_field(q, 1u << 24) == q);
    CHECK(opaque_depth_field(maximum_depth, 16) == 0xF00000u);

    // Translucent: back to front
    CHECK(translucent_depth_field(q) == maximum_depth - q);
    CHECK(translucent_depth_field(0) == maximum_depth);
    CHECK(translucent_depth_field(maximum_depth) == 0);

    // Overlay: the priority in bits 23–16, then a 16-bit back-to-front depth for translucent items
    CHECK(overlay_depth_field(7, false, q) == (7u << 16));
    CHECK(overlay_depth_field(7, true, q) == ((7u << 16) | (0xFFFFu - (q >> 8))));
    CHECK(overlay_depth_field(255, true, 0) == maximum_depth);
    CHECK(overlay_depth_field(0, true, maximum_depth) == 0);
    // A higher priority draws later whatever the depth; within one priority, farther draws first
    CHECK(overlay_depth_field(1, true, maximum_depth) > overlay_depth_field(0, true, 0));
    CHECK(overlay_depth_field(1, true, quantize_depth(100.0f)) < overlay_depth_field(1, true, quantize_depth(10.0f)));

    // Fullscreen effects: the stacking index
    CHECK(fullscreen_effects_depth_field(0) == 0);
    CHECK(fullscreen_effects_depth_field(3) == 3);
}

TEST_CASE("prefix ranges cover exactly the keys of their prefix", "[renderer][unit]")
{
    const key_range opaque = prefix_range(key_prefix(camera_world()));
    CHECK(opaque.contains(draw(camera_world(), 0)));
    CHECK(opaque.contains(draw(camera_world(), maximum_depth, 4095, 131071)));
    CHECK(
        opaque.contains(make_command_key({ camera_world(), 31, callback_command{ (std::uint64_t{ 1 } << 47) - 1 } })));
    CHECK_FALSE(opaque.contains(draw(camera_world(translucency::blended), 0)));
    CHECK(opaque.first == draw(camera_world(), 0));
    CHECK(opaque.last + 1 == draw(camera_world(translucency::blended), 0));

    // blended … subtractive, for forward_transparent
    const key_range translucent = prefix_range(
        key_prefix(camera_world(translucency::blended)), key_prefix(camera_world(translucency::subtractive)));
    CHECK_FALSE(translucent.contains(draw(camera_world(), maximum_depth, 4095, 131071)));
    CHECK(translucent.contains(draw(camera_world(translucency::blended), 0)));
    CHECK(translucent.contains(draw(camera_world(translucency::additive), 17)));
    CHECK(translucent.contains(draw(camera_world(translucency::subtractive), maximum_depth, 4095, 131071)));

    // Partial prefixes: everything in a viewport layer, a viewport or a fullscreen layer
    const key_group overlay{
        fullscreen_layer::scene, viewport::camera(), viewport_layer::overlay, translucency::additive
    };
    const key_range overlays = prefix_range(
        key_prefix(fullscreen_layer::scene, viewport::camera(), viewport_layer::overlay),
        key_prefix(fullscreen_layer::scene, viewport::camera(), viewport_layer::overlay_on_top));
    CHECK(overlays.contains(draw(overlay, 5)));
    CHECK_FALSE(overlays.contains(draw(camera_world(translucency::subtractive), 5)));

    const key_group cascade_2{
        fullscreen_layer::scene, viewport::shadow_cascade(2), viewport_layer::world, translucency::opaque
    };
    const key_range cascades = prefix_range(
        key_prefix(fullscreen_layer::scene, viewport::shadow_cascade(0)),
        key_prefix(fullscreen_layer::scene, viewport::shadow_cascade(maximum_shadow_cascades - 1)));
    CHECK(cascades.contains(draw(cascade_2, 9)));
    CHECK_FALSE(cascades.contains(draw(overlay, 9)));

    const key_group blit{
        fullscreen_layer::fullscreen_effects, viewport::camera(), viewport_layer::background, translucency::blended
    };
    const key_range effects = prefix_range(key_prefix(fullscreen_layer::fullscreen_effects, viewport::camera()));
    CHECK(effects.contains(draw(blit, 2)));
    CHECK_FALSE(effects.contains(draw(cascade_2, 2)));
    CHECK(
        prefix_range(key_prefix(fullscreen_layer::hud)) ==
        key_range{ std::uint64_t{ 2 } << 62, (std::uint64_t{ 3 } << 62) - 1 });
}
