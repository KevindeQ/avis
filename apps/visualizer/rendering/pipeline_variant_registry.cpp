#include "rendering/pipeline_variant_registry.h"

#include "rendering/cascades.h"

#include "avis/graphics/error.h"
#include "avis/graphics/pipeline_cache.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_library.h"
#include "avis/log.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "renderer";

        using attribute_flags = avis::flags<scene::vertex_attribute>;

        // The attributes the shader permutations distinguish; normals and tangents are read by the mesh's mask
        const attribute_flags permuted_attributes =
            attribute_flags{ scene::vertex_attribute::uv0 } | attribute_flags{ scene::vertex_attribute::color0 };

        bool has(const attribute_flags attributes, const scene::vertex_attribute attribute) noexcept
        {
            return static_cast<bool>(attributes & attribute_flags{ attribute });
        }

        bool is_picking(const pass_kind kind) noexcept
        {
            return kind == pass_kind::gbuffer_picking || kind == pass_kind::forward_picking ||
                   kind == pass_kind::overlay_picking || kind == pass_kind::overlay_on_top_picking;
        }

        bool is_overlay(const pass_kind kind) noexcept
        {
            return kind == pass_kind::overlay || kind == pass_kind::overlay_picking ||
                   kind == pass_kind::overlay_on_top || kind == pass_kind::overlay_on_top_picking;
        }

        bool is_depth_only(const pass_kind kind) noexcept
        {
            return kind == pass_kind::depth || kind == pass_kind::shadow;
        }

        bool is_gbuffer(const pass_kind kind) noexcept
        {
            return kind == pass_kind::gbuffer || kind == pass_kind::gbuffer_picking;
        }

        bool is_forward(const pass_kind kind) noexcept
        {
            return kind == pass_kind::forward || kind == pass_kind::forward_picking;
        }

        // The key with only what `kind`'s PSO depends on, so sets that differ elsewhere share it
        pipeline_set_key reduced(const pass_kind kind, const pipeline_set_key& key) noexcept
        {
            pipeline_set_key result = key.canonical();
            result.attributes = result.attributes & permuted_attributes;
            if (is_forward(kind))
            {
                return result; // the shading model picks the pixel shader
            }
            // The G-buffer writes the shading model as a value (11 §5); overlays are always unlit (11 §3.4)
            result.shading_model = scene::shading_model::lit;
            if (is_depth_only(kind) && result.alpha_mode == scene::alpha_mode::opaque)
            {
                result.attributes = attribute_flags{ avis::empty }; // positions only
            }
            return result;
        }

        // ---- shader ids ----

        std::string lower(std::string text)
        {
            std::ranges::transform(
                text,
                text.begin(),
                [](const char character)
                { return static_cast<char>(std::tolower(static_cast<unsigned char>(character))); });
            return text;
        }

        // "<file>.<entry>[.<permutation>].<stage>", the permutation joined from the parts present (AvisShaders.cmake)
        std::string shader_id(
            const std::string_view file,
            const std::string_view entry,
            const std::initializer_list<std::string_view> parts,
            const std::string_view stage)
        {
            std::string permutation;
            for (const std::string_view part : parts)
            {
                if (part.empty())
                {
                    continue;
                }
                if (!permutation.empty())
                {
                    permutation += '_';
                }
                permutation += part;
            }
            std::string id = std::format("{}.{}", file, entry);
            if (!permutation.empty())
            {
                id += '.';
                id += lower(std::move(permutation));
            }
            id += '.';
            id += stage;
            return id;
        }

        std::string_view topology_part(const scene::mesh_topology topology) noexcept
        {
            switch (topology)
            {
            case scene::mesh_topology::lines:
                return "LINES";
            case scene::mesh_topology::points:
                return "POINTS";
            case scene::mesh_topology::triangles:
                break;
            }
            return {};
        }

        // ---- pipeline state ----

        rhi::blend_state opaque_blend() noexcept
        {
            return {};
        }

        // Premultiplied colour (11 §10.1); the overlay pixel shader premultiplies too
        rhi::blend_state translucent_blend(const scene::blend_operation operation) noexcept
        {
            rhi::blend_state blend{ .enable = true };
            switch (operation)
            {
            case scene::blend_operation::normal:
                blend.source_color = rhi::blend_factor::one;
                blend.destination_color = rhi::blend_factor::inverse_source_alpha;
                blend.source_alpha = rhi::blend_factor::one;
                blend.destination_alpha = rhi::blend_factor::inverse_source_alpha;
                break;
            case scene::blend_operation::additive:
                blend.source_color = rhi::blend_factor::one;
                blend.destination_color = rhi::blend_factor::one;
                blend.source_alpha = rhi::blend_factor::zero; // the destination's alpha stays
                blend.destination_alpha = rhi::blend_factor::one;
                break;
            case scene::blend_operation::subtractive:
                blend.source_color = rhi::blend_factor::one;
                blend.destination_color = rhi::blend_factor::one;
                blend.color_operation = rhi::blend_operation::reverse_subtract; // destination − source
                blend.source_alpha = rhi::blend_factor::zero;
                blend.destination_alpha = rhi::blend_factor::one;
                break;
            }
            return blend;
        }

        constexpr std::array gbuffer_targets{ rhi::format::rgba8_unorm_srgb,
                                              rhi::format::rg16_unorm,
                                              rhi::format::rgba8_unorm,
                                              rhi::format::r11g11b10_float,
                                              rhi::format::rg16_float };
        constexpr std::array gbuffer_picking_targets{ rhi::format::rgba8_unorm_srgb, rhi::format::rg16_unorm,
                                                      rhi::format::rgba8_unorm,      rhi::format::r11g11b10_float,
                                                      rhi::format::rg16_float,       rhi::format::rg32_uint };
        constexpr std::array forward_targets{ rhi::format::rgba16_float, rhi::format::r8_unorm };
        constexpr std::array forward_picking_targets{ rhi::format::rgba16_float,
                                                      rhi::format::r8_unorm,
                                                      rhi::format::rg32_uint };
        constexpr std::array overlay_targets{ rhi::format::rgba8_unorm };
        constexpr std::array overlay_picking_targets{ rhi::format::rgba8_unorm, rhi::format::rg32_uint };

        // A recipe: the reduced key of one pass kind, with a write mask. Equal recipes have equal PSO descriptions.
        struct recipe
        {
            pass_kind kind = pass_kind::depth;
            pipeline_set_key key;
            std::uint8_t mask = write_mask::all;

            friend bool operator==(const recipe&, const recipe&) = default;
        };

        struct recipe_hash
        {
            std::size_t operator()(const recipe& value) const noexcept
            {
                return pipeline_set_key_hash{}(value.key) * 31 + static_cast<std::size_t>(value.kind) * 257 +
                       value.mask;
            }
        };

        // The base attribute masks in index order: the common ones first (11.7), then the others by value
        std::vector<attribute_flags> base_attribute_masks()
        {
            using enum scene::vertex_attribute;
            const attribute_flags n{ normal };
            const attribute_flags t{ tangent };
            const attribute_flags uv{ uv0 };
            const attribute_flags c{ color0 };
            std::vector<attribute_flags> masks{
                n | uv, n | t | uv, n, attribute_flags{ avis::empty }, n | uv | c, n | t | uv | c, n | c, c,
            };
            for (std::uint8_t value = 0; value < 16; ++value)
            {
                attribute_flags mask{ avis::empty };
                mask.set_underlying_value(value);
                if (std::ranges::find(masks, mask) == masks.end())
                {
                    masks.push_back(mask);
                }
            }
            return masks;
        }
    } // namespace

    std::string_view to_string(const pass_kind kind) noexcept
    {
        switch (kind)
        {
        case pass_kind::depth:
            return "depth";
        case pass_kind::shadow:
            return "shadow";
        case pass_kind::gbuffer:
            return "gbuffer";
        case pass_kind::gbuffer_picking:
            return "gbuffer_picking";
        case pass_kind::forward:
            return "forward";
        case pass_kind::forward_picking:
            return "forward_picking";
        case pass_kind::overlay:
            return "overlay";
        case pass_kind::overlay_picking:
            return "overlay_picking";
        case pass_kind::overlay_on_top:
            return "overlay_on_top";
        case pass_kind::overlay_on_top_picking:
            return "overlay_on_top_picking";
        }
        return "invalid";
    }

    pass_attachment_formats attachment_formats(const pass_kind kind) noexcept
    {
        switch (kind)
        {
        case pass_kind::depth:
        case pass_kind::shadow:
            return {};
        case pass_kind::gbuffer:
            return { gbuffer_targets };
        case pass_kind::gbuffer_picking:
            return { gbuffer_picking_targets };
        case pass_kind::forward:
            return { forward_targets };
        case pass_kind::forward_picking:
            return { forward_picking_targets };
        case pass_kind::overlay:
        case pass_kind::overlay_on_top:
            return { overlay_targets };
        case pass_kind::overlay_picking:
        case pass_kind::overlay_on_top_picking:
            return { overlay_picking_targets };
        }
        return {};
    }

    pipeline_set_key pipeline_set_key::canonical() const noexcept
    {
        pipeline_set_key result = *this;
        if (result.alpha_mode != scene::alpha_mode::blend)
        {
            result.blend_operation = scene::blend_operation::normal;
        }
        return result;
    }

    std::size_t pipeline_set_key_hash::operator()(const pipeline_set_key& key) const noexcept
    {
        std::size_t value = static_cast<std::size_t>(key.shading_model);
        value = value * 4 + static_cast<std::size_t>(key.alpha_mode);
        value = value * 4 + static_cast<std::size_t>(key.blend_operation);
        value = value * 4 + static_cast<std::size_t>(key.topology);
        value = value * 16 + static_cast<std::size_t>(key.attributes.underlying_value());
        value = value * 2 + (key.double_sided ? 1 : 0);
        value = value * 2 + (key.screen_aligned ? 1 : 0);
        return std::hash<std::size_t>{}(value);
    }

    std::string to_string(const pipeline_set_key& key)
    {
        const std::string_view shading = key.shading_model == scene::shading_model::lit ? "lit" : "unlit";
        std::string_view alpha = "opaque";
        if (key.alpha_mode == scene::alpha_mode::mask)
        {
            alpha = "mask";
        }
        else if (key.alpha_mode == scene::alpha_mode::blend)
        {
            switch (key.blend_operation)
            {
            case scene::blend_operation::normal:
                alpha = "blended";
                break;
            case scene::blend_operation::additive:
                alpha = "additive";
                break;
            case scene::blend_operation::subtractive:
                alpha = "subtractive";
                break;
            }
        }
        std::string_view topology = "triangles";
        if (key.topology == scene::mesh_topology::lines)
        {
            topology = "lines";
        }
        else if (key.topology == scene::mesh_topology::points)
        {
            topology = "points";
        }
        std::string attributes;
        const auto add = [&](const scene::vertex_attribute attribute, const std::string_view name)
        {
            if (has(key.attributes, attribute))
            {
                attributes += attributes.empty() ? "" : "|";
                attributes += name;
            }
        };
        add(scene::vertex_attribute::normal, "normal");
        add(scene::vertex_attribute::tangent, "tangent");
        add(scene::vertex_attribute::uv0, "uv0");
        add(scene::vertex_attribute::color0, "color0");
        return std::format(
            "{} {} {} {} {}{}",
            shading,
            alpha,
            topology,
            attributes.empty() ? "position" : attributes,
            key.double_sided ? "double-sided" : "single-sided",
            key.screen_aligned ? " screen-aligned" : "");
    }

    bool draws(const pass_kind kind, const pipeline_set_key& key) noexcept
    {
        if (is_overlay(kind))
        {
            return true;
        }
        if (key.screen_aligned)
        {
            return false; // only valid in the overlay layers (11 §3.4)
        }
        if (is_forward(kind))
        {
            return key.alpha_mode == scene::alpha_mode::blend;
        }
        return key.alpha_mode != scene::alpha_mode::blend; // depth, shadow, G-buffer
    }

    pass_shader_ids shader_ids_for(const pass_kind kind, const pipeline_set_key& key)
    {
        const pipeline_set_key reduced_key = reduced(kind, key);
        const bool mask = reduced_key.alpha_mode == scene::alpha_mode::mask;
        const bool points = reduced_key.topology == scene::mesh_topology::points;
        pass_shader_ids ids;
        ids.vertex = shader_id(
            "scene_vertex",
            "vertex_main",
            { reduced_key.screen_aligned ? "SCREEN_ALIGNED" : "",
              topology_part(reduced_key.topology),
              has(reduced_key.attributes, scene::vertex_attribute::uv0) ? "UV" : "",
              has(reduced_key.attributes, scene::vertex_attribute::color0) ? "COLOR" : "" },
            "vs");

        const std::string_view picking = is_picking(kind) ? "PICKING" : "";
        if (is_depth_only(kind))
        {
            // Opaque triangles and lines are depth-only
            if (mask || points)
            {
                ids.pixel = shader_id(
                    "depth_prepass", "pixel_main", { points ? "POINTS" : "", mask ? "ALPHA_MASK" : "" }, "ps");
            }
        }
        else if (is_gbuffer(kind))
        {
            ids.pixel =
                shader_id("gbuffer", "pixel_main", { points ? "POINTS" : "", mask ? "ALPHA_MASK" : "", picking }, "ps");
        }
        else if (is_forward(kind))
        {
            ids.pixel = shader_id(
                "forward_transparent",
                "pixel_main",
                { topology_part(reduced_key.topology),
                  reduced_key.shading_model == scene::shading_model::unlit ? "UNLIT" : "",
                  picking },
                "ps");
        }
        else
        {
            ids.pixel = shader_id(
                "overlay",
                "pixel_main",
                { topology_part(reduced_key.topology), mask ? "ALPHA_MASK" : "", picking },
                "ps");
        }
        return ids;
    }

    std::vector<std::string> pipeline_variant_shader_ids()
    {
        std::vector<std::string> ids;
        const auto add = [&ids](std::string id)
        {
            if (!id.empty() && std::ranges::find(ids, id) == ids.end())
            {
                ids.push_back(std::move(id));
            }
        };
        for (const pipeline_set_key& base : pipeline_variant_registry::base_keys())
        {
            for (const bool screen_aligned : { false, true })
            {
                pipeline_set_key key = base;
                key.screen_aligned = screen_aligned;
                for (const pass_kind kind : all_pass_kinds)
                {
                    if (draws(kind, key))
                    {
                        pass_shader_ids pass = shader_ids_for(kind, key);
                        add(std::move(pass.vertex));
                        add(std::move(pass.pixel));
                    }
                }
            }
        }
        return ids;
    }

    rhi::graphics_pipeline_state
        pipeline_state_for(const pass_kind kind, const pipeline_set_key& key, const std::uint8_t mask)
    {
        const pipeline_set_key canonical_key = key.canonical();
        rhi::graphics_pipeline_state state;
        state.rasterizer.cull = canonical_key.double_sided ? rhi::cull_mode::none : rhi::cull_mode::back;
        state.rasterizer.front_counter_clockwise = true;
        // Lines and points are expanded into quads by the vertex shader (11 §9)
        state.topology = rhi::primitive_topology::triangle_list;

        const pass_attachment_formats formats = attachment_formats(kind);
        state.depth_format = formats.depth;
        state.render_target_count = static_cast<std::uint32_t>(formats.render_targets.size());
        std::ranges::copy(formats.render_targets, state.render_target_formats.begin());

        switch (kind)
        {
        case pass_kind::depth:
            state.depth = { .test = true, .write = true, .compare = rhi::comparison::greater_equal };
            break;
        case pass_kind::shadow:
            state.depth = { .test = true, .write = true, .compare = rhi::comparison::greater_equal };
            // Reverse-Z: a negative slope-scaled bias moves the casters away from the sun. Without depth clipping,
            // casters between the sun and the cascade's near plane are flattened onto it (11 §7, cascades.h).
            state.rasterizer.slope_scaled_depth_bias = -shadow_slope_scaled_depth_bias;
            state.rasterizer.depth_clip = false;
            break;
        case pass_kind::gbuffer:
        case pass_kind::gbuffer_picking:
            // The prepass wrote the depth: shade only the visible surface, without overdraw (11 §5)
            state.depth = { .test = true, .write = false, .compare = rhi::comparison::equal };
            break;
        case pass_kind::forward:
        case pass_kind::forward_picking:
        case pass_kind::overlay:
        case pass_kind::overlay_picking:
            state.depth = { .test = true, .write = false, .compare = rhi::comparison::greater_equal };
            break;
        case pass_kind::overlay_on_top:
        case pass_kind::overlay_on_top_picking:
            state.depth = { .test = false, .write = false, .compare = rhi::comparison::always };
            break;
        }

        if (is_forward(kind))
        {
            state.blend[0] = translucent_blend(canonical_key.blend_operation);
            // The reactive mask keeps the most reactive transparent of a pixel
            state.blend[1] = rhi::blend_state{ .enable = true,
                                               .source_color = rhi::blend_factor::one,
                                               .destination_color = rhi::blend_factor::one,
                                               .color_operation = rhi::blend_operation::maximum,
                                               .source_alpha = rhi::blend_factor::one,
                                               .destination_alpha = rhi::blend_factor::one,
                                               .alpha_operation = rhi::blend_operation::maximum };
        }
        else if (is_overlay(kind))
        {
            state.blend[0] = canonical_key.alpha_mode == scene::alpha_mode::blend ?
                                 translucent_blend(canonical_key.blend_operation) :
                                 opaque_blend();
        }
        // The picking target is never blended (11 §5.1); the blend states default to disabled

        // set_write_mask only takes writes away (11 §4.7)
        for (std::uint32_t target = 0; target < state.render_target_count; ++target)
        {
            state.blend[target].write_mask = static_cast<std::uint8_t>(mask & write_mask::color);
        }
        state.depth.write = state.depth.write && (mask & write_mask::depth) != 0;
        return state;
    }

    // ---- registry ----

    pipeline_variant_registry::pipeline_variant_registry(
        graphics::pipeline_cache& cache,
        const graphics::root_signature_registry& root_signatures,
        const graphics::shader_library& shaders) :
        cache_{ cache },
        root_signatures_{ root_signatures },
        shaders_{ shaders }
    {}

    pipeline_variant_registry::~pipeline_variant_registry() = default;

    result<std::unique_ptr<pipeline_variant_registry>> pipeline_variant_registry::create(
        graphics::pipeline_cache& cache,
        const graphics::root_signature_registry& root_signatures,
        const graphics::shader_library& shaders)
    {
        std::unique_ptr<pipeline_variant_registry> registry{ new pipeline_variant_registry{
            cache, root_signatures, shaders } };
        if (auto created = registry->create_base_sets(); !created)
        {
            return std::unexpected{ std::move(created).error() };
        }
        return registry;
    }

    std::vector<pipeline_set_key> pipeline_variant_registry::base_keys()
    {
        struct alpha
        {
            scene::alpha_mode mode;
            scene::blend_operation operation;
        };
        constexpr std::array alphas{
            alpha{ scene::alpha_mode::opaque, scene::blend_operation::normal },
            alpha{ scene::alpha_mode::mask, scene::blend_operation::normal },
            alpha{ scene::alpha_mode::blend, scene::blend_operation::normal },
            alpha{ scene::alpha_mode::blend, scene::blend_operation::additive },
            alpha{ scene::alpha_mode::blend, scene::blend_operation::subtractive },
        };
        constexpr std::array topologies{ scene::mesh_topology::triangles,
                                         scene::mesh_topology::lines,
                                         scene::mesh_topology::points };
        constexpr std::array shading_models{ scene::shading_model::lit, scene::shading_model::unlit };
        const std::vector<attribute_flags> masks = base_attribute_masks();

        std::vector<pipeline_set_key> keys;
        keys.reserve(topologies.size() * alphas.size() * shading_models.size() * masks.size() * 2);
        for (const scene::mesh_topology topology : topologies)
        {
            for (const alpha alpha_value : alphas)
            {
                for (const scene::shading_model shading_model : shading_models)
                {
                    for (const attribute_flags mask : masks)
                    {
                        for (const bool double_sided : { false, true })
                        {
                            keys.push_back(
                                { .shading_model = shading_model,
                                  .alpha_mode = alpha_value.mode,
                                  .blend_operation = alpha_value.operation,
                                  .topology = topology,
                                  .attributes = mask,
                                  .double_sided = double_sided });
                        }
                    }
                }
            }
        }
        return keys;
    }

    result<pipeline_slot> pipeline_variant_registry::create_slot(
        const pass_kind kind, const pipeline_set_key& key, const std::uint8_t mask) const
    {
        const pass_shader_ids ids = shader_ids_for(kind, key);
        const graphics::shader_id vertex = graphics::make_shader_id(ids.vertex);
        const graphics::shader_id pixel =
            ids.pixel.empty() ? graphics::shader_id{} : graphics::make_shader_id(ids.pixel);

        // Matched here rather than by the cache, so the slot knows its signature (11 §4.8)
        std::array<graphics::root_signature_registry::stage_bindings, 2> stages{};
        std::size_t stage_count = 0;
        const auto add_stage = [&](const graphics::shader_id id, const std::string& name) -> result<void>
        {
            const auto entry = shaders_.find(id);
            if (!entry)
            {
                return std::unexpected{ graphics::make_error(
                    graphics::error_kind::shader_not_found,
                    std::format(
                        "pipeline set {} ({}): shader {} isn't in the library",
                        to_string(key),
                        to_string(kind),
                        name)) };
            }
            if (entry->bindings == nullptr)
            {
                return std::unexpected{ graphics::make_error(
                    graphics::error_kind::shader_bindings_missing,
                    std::format("pipeline set {}: shader {} has no bindings", to_string(key), name)) };
            }
            stages[stage_count++] = { entry->stage, entry->bindings };
            return {};
        };
        if (auto added = add_stage(vertex, ids.vertex); !added)
        {
            return std::unexpected{ std::move(added).error() };
        }
        if (!ids.pixel.empty())
        {
            if (auto added = add_stage(pixel, ids.pixel); !added)
            {
                return std::unexpected{ std::move(added).error() };
            }
        }
        auto signature = root_signatures_.find_compatible(std::span{ stages.data(), stage_count });
        if (!signature)
        {
            return std::unexpected{ std::move(signature).error() };
        }

        const std::string debug_name = std::format("{} {}", to_string(kind), to_string(key));
        auto pipeline = cache_.get_or_create(
            shaders_,
            graphics::graphics_pipeline_description{ .vertex_shader = vertex,
                                                     .pixel_shader = pixel,
                                                     .state = pipeline_state_for(kind, key, mask),
                                                     .root_signature = *signature,
                                                     .debug_name = debug_name });
        if (!pipeline)
        {
            return std::unexpected{ std::move(pipeline).error() };
        }
        return pipeline_slot{ .pipeline = *pipeline, .root_signature = *signature };
    }

    result<pipeline_set> pipeline_variant_registry::create_set(const pipeline_set_key& key, const bool base) const
    {
        pipeline_set set{ .key = key.canonical(), .base = base };
        for (const pass_kind kind : all_pass_kinds)
        {
            if (!draws(kind, set.key))
            {
                continue;
            }
            auto slot = create_slot(kind, set.key, write_mask::all);
            if (!slot)
            {
                return std::unexpected{ std::move(slot).error() };
            }
            set.slots[static_cast<std::size_t>(kind)] = *slot;
        }
        return set;
    }

    std::uint32_t pipeline_variant_registry::append(pipeline_set set)
    {
        const auto index = static_cast<std::uint32_t>(sets_.size());
        for (const pipeline_slot& slot : set.slots)
        {
            if (slot.drawn())
            {
                pipelines_.insert(slot.pipeline);
            }
        }
        indices_.emplace(set.key, index);
        sets_.push_back(std::move(set));
        return index;
    }

    result<void> pipeline_variant_registry::create_base_sets()
    {
        const std::vector<pipeline_set_key> keys = base_keys();

        // The distinct PSOs: sets that only differ in state a pass kind ignores share its PSO
        std::vector<recipe> recipes;
        std::unordered_map<recipe, std::size_t, recipe_hash> recipe_indices;
        std::vector<std::array<std::optional<std::size_t>, pass_kind_count>> set_recipes(keys.size());
        for (std::size_t set = 0; set < keys.size(); ++set)
        {
            for (const pass_kind kind : all_pass_kinds)
            {
                if (!draws(kind, keys[set]))
                {
                    continue;
                }
                const recipe value{ .kind = kind, .key = reduced(kind, keys[set]) };
                const auto [position, inserted] = recipe_indices.try_emplace(value, recipes.size());
                if (inserted)
                {
                    recipes.push_back(value);
                }
                set_recipes[set][static_cast<std::size_t>(kind)] = position->second;
            }
        }

        // One bulk item per PSO: the pipeline cache creates them outside its lock (18 §11)
        std::vector<std::optional<result<pipeline_slot>>> slots(recipes.size());
        const auto create_one = [this, &recipes, &slots](const std::size_t index) noexcept
        {
            try
            {
                slots[index] = create_slot(recipes[index].kind, recipes[index].key, recipes[index].mask);
            }
            catch (...)
            {
                slots[index] = std::unexpected{ avis::make_error(std::current_exception()) };
            }
        };
        if (threading::detail::blocking_wait_forbidden())
        {
            // On a parallel worker or an IO thread a blocking wait could deadlock: create them here
            for (std::size_t index = 0; index < recipes.size(); ++index)
            {
                create_one(index);
            }
        }
        else
        {
            auto created = threading::sync_wait_result(
                stdexec::bulk(
                    stdexec::schedule(stdexec::get_parallel_scheduler()), stdexec::par, recipes.size(), create_one));
            if (!created)
            {
                return std::unexpected{ std::move(created).error() };
            }
        }

        for (const std::optional<result<pipeline_slot>>& slot : slots)
        {
            if (!slot || !*slot)
            {
                return std::unexpected{ slot ? slot->error() :
                                               avis::make_error(
                                                   generic_error_kind::logic_error, "a pipeline wasn't created") };
            }
        }

        for (std::size_t set = 0; set < keys.size(); ++set)
        {
            pipeline_set value{ .key = keys[set].canonical(), .base = true };
            for (std::size_t kind = 0; kind < pass_kind_count; ++kind)
            {
                if (const std::optional<std::size_t> recipe_index = set_recipes[set][kind])
                {
                    value.slots[kind] = **slots[*recipe_index];
                }
            }
            append(std::move(value));
        }
        base_sets_ = sets_.size();
        log::get(logger_name)
            ->info(
                "pipeline variants: {} base sets with {} distinct pipelines (11 §4.4)", base_sets_, pipelines_.size());
        return {};
    }

    std::optional<std::uint32_t> pipeline_variant_registry::find_existing(const pipeline_set_key& key) const
    {
        const auto found = indices_.find(key.canonical());
        if (found == indices_.end())
        {
            return std::nullopt;
        }
        return found->second;
    }

    result<std::uint32_t> pipeline_variant_registry::find(const pipeline_set_key& key)
    {
        if (const std::optional<std::uint32_t> existing = find_existing(key))
        {
            return *existing;
        }
        if (sets_.size() >= maximum_pipeline_sets)
        {
            return std::unexpected{ avis::make_error(
                generic_error_kind::invalid_configuration,
                std::format(
                    "pipeline set {}: the sort key holds {} pipeline sets, all in use (11 §4.2)",
                    to_string(key.canonical()),
                    maximum_pipeline_sets)) };
        }
        auto set = create_set(key, false);
        if (!set)
        {
            return std::unexpected{ std::move(set).error() };
        }
        const std::uint32_t index = append(std::move(*set));
        log::get(logger_name)
            ->info("pipeline variants: created set {} for {} on first use", index, to_string(sets_[index].key));
        return index;
    }

    result<pipeline_slot> pipeline_variant_registry::write_mask_variant(
        const std::uint32_t index, const pass_kind kind, const std::uint8_t mask)
    {
        if (index >= sets_.size())
        {
            return std::unexpected{ graphics::make_error(
                graphics::error_kind::invalid_argument,
                std::format("write_mask_variant: no pipeline set {} ({} sets)", index, sets_.size())) };
        }
        const pipeline_set& set = sets_[index];
        const pipeline_slot& slot = set.slot(kind);
        const auto effective = static_cast<std::uint8_t>(mask & write_mask::all);
        if (!slot.drawn() || effective == write_mask::all)
        {
            return slot;
        }

        const std::uint64_t cache_key =
            (std::uint64_t{ index } << 16) | (std::uint64_t{ static_cast<std::uint8_t>(kind) } << 8) | effective;
        {
            const std::lock_guard lock{ write_masks_mutex_ };
            if (const auto found = write_masks_.find(cache_key); found != write_masks_.end())
            {
                return found->second;
            }
        }
        // Created outside the lock; the pipeline cache returns the first PSO if two threads race on it
        auto created = create_slot(kind, set.key, effective);
        if (!created)
        {
            return std::unexpected{ std::move(created).error() };
        }
        const std::lock_guard lock{ write_masks_mutex_ };
        return write_masks_.try_emplace(cache_key, *created).first->second;
    }

    pipeline_variant_statistics pipeline_variant_registry::statistics() const
    {
        std::size_t write_mask_variants = 0;
        {
            const std::lock_guard lock{ write_masks_mutex_ };
            write_mask_variants = write_masks_.size();
        }
        return { .sets = sets_.size(),
                 .base_sets = base_sets_,
                 .first_use_sets = sets_.size() - base_sets_,
                 .pipelines = pipelines_.size(),
                 .write_mask_variants = write_mask_variants };
    }
} // namespace avis::visualizer::rendering
