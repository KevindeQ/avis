// The pipeline variant registry (docs/tasks/11-renderer.md §11.7, 11 §4.4, §4.7): the base sets and their order, the
// slots each pass kind fills, the state per pass kind, first-use sets, write-mask variants and the root signature of
// each slot.

#include "helpers/renderer_fixture.h"
#include "rendering/cascades.h"
#include "rendering/pipeline_variant_registry.h"

#include "avis/graphics/pipeline_cache.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_library.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;

namespace
{
    using attribute_flags = avis::flags<scene::vertex_attribute>;

    const attribute_flags normal_uv =
        attribute_flags{ scene::vertex_attribute::normal } | attribute_flags{ scene::vertex_attribute::uv0 };

    // 2 shading models × (opaque, mask, 3 blend operations) × 3 topologies × 16 attribute masks × 2 sidednesses
    constexpr std::size_t base_set_count = 2 * 5 * 3 * 16 * 2;

    bool opaque_or_mask(const pipeline_set_key& key)
    {
        return key.alpha_mode != scene::alpha_mode::blend;
    }

    rhi::root_signature_handle standard_signature(tests::renderer_fixture& fixture)
    {
        const auto vertex = fixture.shaders->find(graphics::make_shader_id("scene_vertex.vertex_main.vs"));
        REQUIRE(vertex.has_value());
        REQUIRE(vertex->bindings != nullptr);
        const graphics::root_signature_registry::stage_bindings stage{ vertex->stage, vertex->bindings };
        auto signature = fixture.root_signatures->find_compatible(std::span{ &stage, 1 });
        REQUIRE(signature.has_value());
        return *signature;
    }
} // namespace

TEST_CASE("the base keys cover the material model, common sets first", "[renderer][unit]")
{
    const std::vector<pipeline_set_key> keys = pipeline_variant_registry::base_keys();
    REQUIRE(keys.size() == base_set_count);
    CHECK(keys.size() <= maximum_pipeline_sets);

    // Lit, opaque, triangles, with normals and uv0, single-sided first
    CHECK(
        keys[0] == pipeline_set_key{ .shading_model = scene::shading_model::lit,
                                     .alpha_mode = scene::alpha_mode::opaque,
                                     .topology = scene::mesh_topology::triangles,
                                     .attributes = normal_uv,
                                     .double_sided = false });
    CHECK(keys[1].double_sided);

    // Every key once, canonical, never screen-aligned
    std::unordered_set<pipeline_set_key, pipeline_set_key_hash> distinct;
    for (const pipeline_set_key& key : keys)
    {
        CHECK(key == key.canonical());
        CHECK_FALSE(key.screen_aligned);
        distinct.insert(key);
    }
    CHECK(distinct.size() == keys.size());

    // Triangles before lines before points; within a topology, opaque before mask before blend
    const auto topology_rank = [](const pipeline_set_key& key) { return static_cast<int>(key.topology); };
    CHECK(std::ranges::is_sorted(keys, {}, topology_rank));
    const auto first_lines = std::ranges::find(keys, scene::mesh_topology::lines, &pipeline_set_key::topology);
    CHECK(
        std::ranges::is_sorted(
            std::ranges::subrange(keys.begin(), first_lines),
            {},
            [](const pipeline_set_key& key)
            {
                return key.alpha_mode == scene::alpha_mode::blend ? 2 + static_cast<int>(key.blend_operation) :
                                                                    static_cast<int>(key.alpha_mode);
            }));
}

TEST_CASE("canonical keys ignore the blend operation outside alpha_mode blend", "[renderer][unit]")
{
    const pipeline_set_key opaque{ .alpha_mode = scene::alpha_mode::opaque,
                                   .blend_operation = scene::blend_operation::additive };
    CHECK(opaque.canonical().blend_operation == scene::blend_operation::normal);
    const pipeline_set_key additive{ .alpha_mode = scene::alpha_mode::blend,
                                     .blend_operation = scene::blend_operation::additive };
    CHECK(additive.canonical() == additive);
    CHECK(to_string(additive) == "lit additive triangles position single-sided");
}

TEST_CASE("each pass kind draws the keys of its layer", "[renderer][unit]")
{
    const pipeline_set_key opaque{};
    const pipeline_set_key mask{ .alpha_mode = scene::alpha_mode::mask };
    const pipeline_set_key blend{ .alpha_mode = scene::alpha_mode::blend };
    pipeline_set_key label = opaque;
    label.screen_aligned = true;

    for (const pass_kind kind : { pass_kind::depth, pass_kind::shadow, pass_kind::gbuffer, pass_kind::gbuffer_picking })
    {
        CHECK(draws(kind, opaque));
        CHECK(draws(kind, mask));
        CHECK_FALSE(draws(kind, blend));
        CHECK_FALSE(draws(kind, label));
    }
    for (const pass_kind kind : { pass_kind::forward, pass_kind::forward_picking })
    {
        CHECK_FALSE(draws(kind, opaque));
        CHECK(draws(kind, blend));
        CHECK_FALSE(draws(kind, label));
    }
    for (const pass_kind kind : { pass_kind::overlay,
                                  pass_kind::overlay_picking,
                                  pass_kind::overlay_on_top,
                                  pass_kind::overlay_on_top_picking })
    {
        CHECK(draws(kind, opaque));
        CHECK(draws(kind, blend));
        CHECK(draws(kind, label));
    }
}

TEST_CASE("pipeline state per pass kind", "[renderer][unit]")
{
    const pipeline_set_key opaque{ .attributes = normal_uv };

    SECTION("depth and shadow: depth only, reverse-Z, write")
    {
        for (const pass_kind kind : { pass_kind::depth, pass_kind::shadow })
        {
            const rhi::graphics_pipeline_state state = pipeline_state_for(kind, opaque, write_mask::all);
            CHECK(state.render_target_count == 0);
            CHECK(state.depth_format == rhi::format::d32_float);
            CHECK(state.depth.test);
            CHECK(state.depth.write);
            CHECK(state.depth.compare == rhi::comparison::greater_equal);
            CHECK(state.rasterizer.cull == rhi::cull_mode::back);
            CHECK(state.topology == rhi::primitive_topology::triangle_list);
        }
        // Only the shadow PSO has a bias (away from the sun: negative, reverse-Z) and pancaking (11.12)
        const rhi::graphics_pipeline_state depth = pipeline_state_for(pass_kind::depth, opaque, write_mask::all);
        const rhi::graphics_pipeline_state shadow = pipeline_state_for(pass_kind::shadow, opaque, write_mask::all);
        CHECK(depth.rasterizer.slope_scaled_depth_bias == 0.0f);
        CHECK(depth.rasterizer.depth_clip);
        CHECK(shadow.rasterizer.slope_scaled_depth_bias == -shadow_slope_scaled_depth_bias);
        CHECK_FALSE(shadow.rasterizer.depth_clip);
        // Opaque triangles have no pixel shader; masked ones and points do
        CHECK(shader_ids_for(pass_kind::depth, opaque).pixel.empty());
        CHECK(shader_ids_for(pass_kind::depth, opaque).vertex == "scene_vertex.vertex_main.vs");
        CHECK(
            shader_ids_for(pass_kind::depth, { .alpha_mode = scene::alpha_mode::mask, .attributes = normal_uv })
                .pixel == "depth_prepass.pixel_main.alpha_mask.ps");
        CHECK(
            shader_ids_for(pass_kind::shadow, { .topology = scene::mesh_topology::points }).pixel ==
            "depth_prepass.pixel_main.points.ps");
    }

    SECTION("G-buffer: depth equal without writes, five targets, picking adds the id")
    {
        const rhi::graphics_pipeline_state state = pipeline_state_for(pass_kind::gbuffer, opaque, write_mask::all);
        CHECK(state.depth.compare == rhi::comparison::equal);
        CHECK_FALSE(state.depth.write);
        REQUIRE(state.render_target_count == 5);
        CHECK(state.render_target_formats[0] == rhi::format::rgba8_unorm_srgb);
        CHECK(state.render_target_formats[1] == rhi::format::rg16_unorm);
        CHECK(state.render_target_formats[2] == rhi::format::rgba8_unorm);
        CHECK(state.render_target_formats[3] == rhi::format::r11g11b10_float);
        CHECK(state.render_target_formats[4] == rhi::format::rg16_float);
        CHECK_FALSE(state.blend[0].enable);

        const rhi::graphics_pipeline_state picking =
            pipeline_state_for(pass_kind::gbuffer_picking, opaque, write_mask::all);
        REQUIRE(picking.render_target_count == 6);
        CHECK(picking.render_target_formats[5] == rhi::format::rg32_uint);
        CHECK_FALSE(picking.blend[5].enable);

        CHECK(shader_ids_for(pass_kind::gbuffer, opaque).vertex == "scene_vertex.vertex_main.uv.vs");
        CHECK(shader_ids_for(pass_kind::gbuffer_picking, opaque).pixel == "gbuffer.pixel_main.picking.ps");
    }

    SECTION("forward: blend state from the blend operation")
    {
        pipeline_set_key blend{ .alpha_mode = scene::alpha_mode::blend };
        rhi::graphics_pipeline_state state = pipeline_state_for(pass_kind::forward, blend, write_mask::all);
        CHECK(state.depth.test);
        CHECK_FALSE(state.depth.write);
        REQUIRE(state.render_target_count == 2);
        CHECK(state.render_target_formats[0] == rhi::format::rgba16_float);
        CHECK(state.render_target_formats[1] == rhi::format::r8_unorm);
        // Premultiplied alpha
        CHECK(state.blend[0].enable);
        CHECK(state.blend[0].source_color == rhi::blend_factor::one);
        CHECK(state.blend[0].destination_color == rhi::blend_factor::inverse_source_alpha);
        CHECK(state.blend[0].color_operation == rhi::blend_operation::add);
        // The reactive mask keeps the maximum
        CHECK(state.blend[1].color_operation == rhi::blend_operation::maximum);

        blend.blend_operation = scene::blend_operation::additive;
        state = pipeline_state_for(pass_kind::forward, blend, write_mask::all);
        CHECK(state.blend[0].destination_color == rhi::blend_factor::one);
        CHECK(state.blend[0].color_operation == rhi::blend_operation::add);

        blend.blend_operation = scene::blend_operation::subtractive;
        state = pipeline_state_for(pass_kind::forward, blend, write_mask::all);
        CHECK(state.blend[0].destination_color == rhi::blend_factor::one);
        CHECK(state.blend[0].color_operation == rhi::blend_operation::reverse_subtract);

        blend.shading_model = scene::shading_model::unlit;
        blend.topology = scene::mesh_topology::lines;
        CHECK(
            shader_ids_for(pass_kind::forward_picking, blend).pixel ==
            "forward_transparent.pixel_main.lines_unlit_picking.ps");
    }

    SECTION("overlay: depth test without writes; on top: no test")
    {
        const rhi::graphics_pipeline_state overlay = pipeline_state_for(pass_kind::overlay, opaque, write_mask::all);
        CHECK(overlay.depth.test);
        CHECK_FALSE(overlay.depth.write);
        REQUIRE(overlay.render_target_count == 1);
        CHECK(overlay.render_target_formats[0] == rhi::format::rgba8_unorm);
        CHECK_FALSE(overlay.blend[0].enable);

        const rhi::graphics_pipeline_state on_top =
            pipeline_state_for(pass_kind::overlay_on_top, { .alpha_mode = scene::alpha_mode::blend }, write_mask::all);
        CHECK_FALSE(on_top.depth.test);
        CHECK(on_top.blend[0].enable);

        pipeline_set_key label{ .topology = scene::mesh_topology::triangles,
                                .attributes = attribute_flags{ scene::vertex_attribute::uv0 } |
                                              attribute_flags{ scene::vertex_attribute::color0 },
                                .screen_aligned = true };
        CHECK(
            shader_ids_for(pass_kind::overlay, label).vertex == "scene_vertex.vertex_main.screen_aligned_uv_color.vs");
        CHECK(shader_ids_for(pass_kind::overlay_picking, label).pixel == "overlay.pixel_main.picking.ps");
    }

    SECTION("sidedness and write masks")
    {
        pipeline_set_key double_sided = opaque;
        double_sided.double_sided = true;
        CHECK(
            pipeline_state_for(pass_kind::depth, double_sided, write_mask::all).rasterizer.cull ==
            rhi::cull_mode::none);

        const rhi::graphics_pipeline_state color_only = pipeline_state_for(pass_kind::depth, opaque, write_mask::color);
        CHECK_FALSE(color_only.depth.write);
        const rhi::graphics_pipeline_state red =
            pipeline_state_for(pass_kind::gbuffer_picking, opaque, write_mask::red | write_mask::depth);
        for (std::uint32_t target = 0; target < red.render_target_count; ++target)
        {
            CHECK(red.blend[target].write_mask == write_mask::red);
        }
        // A mask can't turn on depth writes the pass doesn't have
        CHECK_FALSE(red.depth.write);
    }
}

TEST_CASE("startup creates the base pipeline sets", "[renderer]")
{
    tests::renderer_fixture fixture;
    const pipeline_variant_registry& registry = fixture.renderer->pipeline_variants();
    const std::vector<pipeline_set_key> keys = pipeline_variant_registry::base_keys();

    REQUIRE(registry.size() == base_set_count);
    const pipeline_variant_statistics statistics = registry.statistics();
    CHECK(statistics.base_sets == base_set_count);
    CHECK(statistics.first_use_sets == 0);
    CHECK(statistics.pipelines > 0);
    // Sets share the PSOs of the state a pass kind ignores
    CHECK(statistics.pipelines < base_set_count);
    CHECK(fixture.renderer->debug_information().pipelines.sets == base_set_count);

    for (std::uint32_t index = 0; index < registry.size(); ++index)
    {
        const pipeline_set& set = registry.set(index);
        CHECK(set.key == keys[index]);
        CHECK(set.base);
        CHECK(registry.find_existing(set.key) == index);
        // The depth, shadow and G-buffer slots are filled exactly for the opaque and mask keys
        CHECK(set.slot(pass_kind::depth).drawn() == opaque_or_mask(set.key));
        CHECK(set.slot(pass_kind::shadow).drawn() == opaque_or_mask(set.key));
        CHECK(set.slot(pass_kind::gbuffer).drawn() == opaque_or_mask(set.key));
        CHECK(set.slot(pass_kind::gbuffer_picking).drawn() == opaque_or_mask(set.key));
        CHECK(set.slot(pass_kind::forward).drawn() == !opaque_or_mask(set.key));
        CHECK(set.slot(pass_kind::forward_picking).drawn() == !opaque_or_mask(set.key));
        for (const pass_kind kind : { pass_kind::overlay,
                                      pass_kind::overlay_picking,
                                      pass_kind::overlay_on_top,
                                      pass_kind::overlay_on_top_picking })
        {
            CHECK(set.slot(kind).drawn());
        }
    }

    // Lit and unlit sets share their depth prepass PSO
    const pipeline_set_key lit = keys[0];
    pipeline_set_key unlit = lit;
    unlit.shading_model = scene::shading_model::unlit;
    const std::optional<std::uint32_t> unlit_index = registry.find_existing(unlit);
    REQUIRE(unlit_index.has_value());
    CHECK(registry.set(*unlit_index).slot(pass_kind::depth) == registry.set(0).slot(pass_kind::depth));
    CHECK(registry.set(*unlit_index).slot(pass_kind::gbuffer) == registry.set(0).slot(pass_kind::gbuffer));
}

TEST_CASE("every pipeline set shader is in the built library", "[renderer]")
{
    tests::renderer_fixture fixture;
    const std::vector<std::string> ids = pipeline_variant_shader_ids();
    CHECK(ids.size() > 0);
    for (const std::string& id : ids)
    {
        INFO(id);
        CHECK(fixture.shaders->find(graphics::make_shader_id(id)).has_value());
    }
}

TEST_CASE("each slot records the root signature its PSO was matched to", "[renderer]")
{
    tests::renderer_fixture fixture;
    const rhi::root_signature_handle standard = standard_signature(fixture);
    const pipeline_variant_registry& registry = fixture.renderer->pipeline_variants();
    for (std::uint32_t index = 0; index < registry.size(); ++index)
    {
        for (const pipeline_slot& slot : registry.set(index).slots)
        {
            if (slot.drawn())
            {
                CHECK(slot.root_signature == standard);
            }
            else
            {
                CHECK_FALSE(slot.root_signature.is_valid());
            }
        }
    }
}

TEST_CASE("set indices are stable across two renderer creations", "[renderer]")
{
    tests::renderer_fixture fixture;
    const pipeline_set_key label{ .shading_model = scene::shading_model::unlit,
                                  .attributes = attribute_flags{ scene::vertex_attribute::uv0 },
                                  .screen_aligned = true };

    std::vector<pipeline_set_key> first;
    {
        pipeline_variant_registry& registry = fixture.renderer->pipeline_variants();
        REQUIRE(registry.find(label).has_value());
        for (std::uint32_t index = 0; index < registry.size(); ++index)
        {
            first.push_back(registry.set(index).key);
        }
    }

    fixture.destroy_renderer();
    fixture.create_renderer();
    pipeline_variant_registry& registry = fixture.renderer->pipeline_variants();
    const result<std::uint32_t> again = registry.find(label);
    REQUIRE(again.has_value());
    REQUIRE(registry.size() == first.size());
    for (std::uint32_t index = 0; index < registry.size(); ++index)
    {
        CHECK(registry.set(index).key == first[index]);
    }
}

TEST_CASE("a key outside the base sets is created on first use", "[renderer]")
{
    tests::renderer_fixture fixture;
    pipeline_variant_registry& registry = fixture.renderer->pipeline_variants();
    const pipeline_set_key label{ .shading_model = scene::shading_model::unlit,
                                  .alpha_mode = scene::alpha_mode::blend,
                                  .attributes = attribute_flags{ scene::vertex_attribute::uv0 },
                                  .double_sided = true,
                                  .screen_aligned = true };
    CHECK_FALSE(registry.find_existing(label).has_value());

    const std::size_t cached_before = fixture.pipeline_cache->size();
    const result<std::uint32_t> created = registry.find(label);
    REQUIRE(created.has_value());
    CHECK(*created == base_set_count);
    const pipeline_set& set = registry.set(*created);
    CHECK_FALSE(set.base);
    CHECK(set.slot(pass_kind::overlay).drawn());
    CHECK(set.slot(pass_kind::overlay_on_top_picking).drawn());
    CHECK_FALSE(set.slot(pass_kind::depth).drawn());
    CHECK_FALSE(set.slot(pass_kind::forward).drawn());
    CHECK(registry.statistics().first_use_sets == 1);
    const std::size_t cached_after = fixture.pipeline_cache->size();
    CHECK(cached_after > cached_before);

    // A second lookup returns the same set without creating a PSO
    const result<std::uint32_t> again = registry.find(label);
    REQUIRE(again.has_value());
    CHECK(*again == *created);
    CHECK(registry.size() == base_set_count + 1);
    CHECK(fixture.pipeline_cache->size() == cached_after);

    // A base key, given with a blend operation it ignores, is its base set
    pipeline_set_key opaque = pipeline_variant_registry::base_keys()[0];
    opaque.blend_operation = scene::blend_operation::subtractive;
    const result<std::uint32_t> base = registry.find(opaque);
    REQUIRE(base.has_value());
    CHECK(*base == 0);
    CHECK(fixture.pipeline_cache->size() == cached_after);
}

TEST_CASE("write-mask variants are created on first use and cached", "[renderer]")
{
    tests::renderer_fixture fixture;
    pipeline_variant_registry& registry = fixture.renderer->pipeline_variants();
    const pipeline_slot depth = registry.set(0).slot(pass_kind::depth);

    // The full mask is the set's own PSO
    const result<pipeline_slot> full = registry.write_mask_variant(0, pass_kind::depth, write_mask::all);
    REQUIRE(full.has_value());
    CHECK(*full == depth);

    const std::size_t cached_before = fixture.pipeline_cache->size();
    const result<pipeline_slot> no_depth = registry.write_mask_variant(0, pass_kind::depth, write_mask::color);
    REQUIRE(no_depth.has_value());
    CHECK(no_depth->drawn());
    CHECK(no_depth->pipeline != depth.pipeline);
    CHECK(no_depth->root_signature == depth.root_signature);
    CHECK(fixture.pipeline_cache->size() == cached_before + 1);
    CHECK(registry.statistics().write_mask_variants == 1);

    const result<pipeline_slot> again = registry.write_mask_variant(0, pass_kind::depth, write_mask::color);
    REQUIRE(again.has_value());
    CHECK(*again == *no_depth);
    CHECK(fixture.pipeline_cache->size() == cached_before + 1);

    // An empty slot stays empty; an index past the sets fails
    const result<pipeline_slot> forward = registry.write_mask_variant(0, pass_kind::forward, write_mask::color);
    REQUIRE(forward.has_value());
    CHECK_FALSE(forward->drawn());
    CHECK_FALSE(
        registry.write_mask_variant(static_cast<std::uint32_t>(registry.size()), pass_kind::depth, 0).has_value());
}
