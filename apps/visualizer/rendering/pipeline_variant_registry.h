#ifndef AVIS_VISUALIZER_RENDERING_PIPELINE_VARIANT_REGISTRY_H
#define AVIS_VISUALIZER_RENDERING_PIPELINE_VARIANT_REGISTRY_H

// Pipeline sets (docs/design/11-renderer.md §4.4, §4.7, docs/tasks/11-renderer.md §11.7, 18 §10, §11, D-106).
//
// A pipeline set is one combination of the state a draw's PSOs depend on (pipeline_set_key). Its 12-bit index is the
// top of the sort key's material field, so draws sort by set. A set holds one PSO per pass kind that draws it, with the
// root signature the PSO was matched to:
//   depth, shadow, gbuffer(_picking)       opaque and mask keys, not screen-aligned
//   forward(_picking)                      blend keys, not screen-aligned
//   overlay(_picking), overlay_on_top(_picking)  every key: an overlay instance may use any material
// Picking kinds add the picking_id target (11 §5.1). A pass kind that doesn't draw a key leaves its slot empty.
//
// Startup creates the base sets: every key the material model produces (2 shading models × 5 alpha modes and blend
// operations × 3 topologies × 16 attribute masks × 2 sidednesses), in a fixed order with the common sets first. Their
// PSOs are created in a bulk on the parallel scheduler through the pipeline cache. Keys that only differ in state a
// pass kind ignores share that kind's PSO: the depth prepass of a lit and an unlit set is the same pipeline. Any other
// key (a screen-aligned one, for instance) gets the next free index on first use, which is logged.
//
// Shaders: scene_vertex.hlsl for every pass, permuted by screen alignment, topology and the uv0 and color0 attributes,
// and one pixel shader per pass family (depth_prepass, gbuffer, forward_transparent, overlay), permuted by topology,
// alpha mask, shading model and picking. Their permutation names are built here and in the shaders' annotations.
//
// Threads: find and the startup run on the frame chain's prepare stage, never during parallel recording.
// write_mask_variant is thread-safe, so a range walker may ask for one while recording.

#include "avis/enum_flags.h"
#include "avis/error.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/types.h"
#include "avis/scene/assets/material.h"
#include "avis/scene/assets/mesh_data.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace avis::graphics
{
    class pipeline_cache;
    class root_signature_registry;
    class shader_library;
} // namespace avis::graphics

namespace avis::visualizer::rendering
{
    // The passes that draw pipeline sets, each with its own PSO per set (11 §4.4, §4.6)
    enum class pass_kind : std::uint8_t
    {
        depth, // depth_prepass: depth only, reverse-Z greater-equal, write
        shadow, // shadow_cascades: depth only (11.12 adds the bias)
        gbuffer, // depth equal, no write; the five G-buffer targets
        gbuffer_picking, // plus picking_id
        forward, // forward_transparent: depth test, no write; hdr_color and reactive_mask
        forward_picking,
        overlay, // depth test against the scene, no write; ldr_color
        overlay_picking,
        overlay_on_top, // no depth test
        overlay_on_top_picking,
    };

    inline constexpr std::size_t pass_kind_count = 10;

    inline constexpr std::array<pass_kind, pass_kind_count> all_pass_kinds{
        pass_kind::depth,          pass_kind::shadow,
        pass_kind::gbuffer,        pass_kind::gbuffer_picking,
        pass_kind::forward,        pass_kind::forward_picking,
        pass_kind::overlay,        pass_kind::overlay_picking,
        pass_kind::overlay_on_top, pass_kind::overlay_on_top_picking,
    };

    std::string_view to_string(pass_kind kind) noexcept;

    // The attachments a pass kind's PSOs are created for
    struct pass_attachment_formats
    {
        std::span<const rhi::format> render_targets;
        rhi::format depth = rhi::format::d32_float;
    };

    pass_attachment_formats attachment_formats(pass_kind kind) noexcept;

    // What a draw's PSOs depend on. blend_operation only counts for alpha_mode blend: find and the registry use the
    // canonical key, which sets it to normal otherwise.
    struct pipeline_set_key
    {
        scene::shading_model shading_model = scene::shading_model::lit;
        scene::alpha_mode alpha_mode = scene::alpha_mode::opaque;
        scene::blend_operation blend_operation = scene::blend_operation::normal;
        scene::mesh_topology topology = scene::mesh_topology::triangles;
        avis::flags<scene::vertex_attribute> attributes{ avis::empty };
        bool double_sided = false;
        bool screen_aligned = false; // instance_flags::screen_aligned; overlay kinds only

        pipeline_set_key canonical() const noexcept;

        friend bool operator==(const pipeline_set_key&, const pipeline_set_key&) = default;
    };

    struct pipeline_set_key_hash
    {
        std::size_t operator()(const pipeline_set_key& key) const noexcept;
    };

    // "lit opaque triangles normal|uv0 single-sided"
    std::string to_string(const pipeline_set_key& key);

    // Whether `kind` draws sets of `key` (the slot is filled exactly then)
    bool draws(pass_kind kind, const pipeline_set_key& key) noexcept;

    // One pass kind's PSO of a set
    struct pipeline_slot
    {
        rhi::graphics_pipeline_handle pipeline; // invalid: the pass doesn't draw the set
        rhi::root_signature_handle root_signature; // the signature the PSO was matched to (18 §10), for the
                                                   // redundant-state filter (11 §4.8)

        bool drawn() const noexcept
        {
            return pipeline.is_valid();
        }

        friend bool operator==(const pipeline_slot&, const pipeline_slot&) = default;
    };

    struct pipeline_set
    {
        pipeline_set_key key; // canonical
        bool base = true; // created at startup; false for a first-use set
        std::array<pipeline_slot, pass_kind_count> slots{};

        const pipeline_slot& slot(const pass_kind kind) const noexcept
        {
            return slots[static_cast<std::size_t>(kind)];
        }
    };

    // The immediate of set_write_mask (11 §4.7): colour channels of every render target, and depth writes. A mask can
    // only take writes away: depth writes stay off in a pass whose PSOs don't write depth.
    namespace write_mask
    {
        inline constexpr std::uint8_t red = 1u << 0;
        inline constexpr std::uint8_t green = 1u << 1;
        inline constexpr std::uint8_t blue = 1u << 2;
        inline constexpr std::uint8_t alpha = 1u << 3;
        inline constexpr std::uint8_t color = red | green | blue | alpha;
        inline constexpr std::uint8_t depth = 1u << 4;
        inline constexpr std::uint8_t all = color | depth;
    } // namespace write_mask

    // The sort key's pipeline set field is 12 bits (11 §4.2)
    inline constexpr std::uint32_t maximum_pipeline_sets = 4096;

    struct pipeline_variant_statistics
    {
        std::size_t sets = 0;
        std::size_t base_sets = 0;
        std::size_t first_use_sets = 0;
        std::size_t pipelines = 0; // distinct PSOs in the sets' slots
        std::size_t write_mask_variants = 0;
    };

    // The shader ids of a set's pass kind: the pixel shader is empty for a depth-only PSO. Exposed for tests.
    struct pass_shader_ids
    {
        std::string vertex;
        std::string pixel;
    };

    pass_shader_ids shader_ids_for(pass_kind kind, const pipeline_set_key& key);

    // Every shader id a pipeline set can use, for the check that the build produced them all
    std::vector<std::string> pipeline_variant_shader_ids();

    // The full description of a set's pass kind PSO with a write mask, except for its root signature. Exposed for
    // tests of the state per pass kind.
    rhi::graphics_pipeline_state pipeline_state_for(pass_kind kind, const pipeline_set_key& key, std::uint8_t mask);

    class pipeline_variant_registry
    {
    public:
        // Creates the base sets. The cache, the root signatures and the library must outlive the registry; the
        // signatures must hold one compatible with the scene shaders (the standard signature). Fails with the first
        // error of the pipeline cache or the signature matching (a missing shader names its id).
        static result<std::unique_ptr<pipeline_variant_registry>> create(
            graphics::pipeline_cache& cache,
            const graphics::root_signature_registry& root_signatures,
            const graphics::shader_library& shaders);

        ~pipeline_variant_registry();
        pipeline_variant_registry(const pipeline_variant_registry&) = delete;
        pipeline_variant_registry& operator=(const pipeline_variant_registry&) = delete;

        // The base keys, canonical, in index order: lit before unlit, opaque before mask before blend, triangles before
        // lines before points, normal|uv0 first among the attribute masks, single-sided before double-sided. The
        // order is fixed, so set indices are stable across runs.
        static std::vector<pipeline_set_key> base_keys();

        // The set of `key` (canonicalised). O(1); a key without a set gets one, created now on the calling thread
        // and logged. Fails when the PSOs can't be created, or past maximum_pipeline_sets. Prepare stage only.
        result<std::uint32_t> find(const pipeline_set_key& key);

        // The set of `key` if it has one; never creates
        std::optional<std::uint32_t> find_existing(const pipeline_set_key& key) const;

        // Valid while the registry lives: sets are never moved or removed
        const pipeline_set& set(std::uint32_t index) const
        {
            return sets_[index];
        }

        std::size_t size() const noexcept
        {
            return sets_.size();
        }

        // `kind`'s PSO of set `index` with `mask` (write_mask), created on first use and cached by (set, kind, mask).
        // write_mask::all is the set's own slot. An empty slot stays empty. Thread-safe.
        result<pipeline_slot> write_mask_variant(std::uint32_t index, pass_kind kind, std::uint8_t mask);

        pipeline_variant_statistics statistics() const;

    private:
        pipeline_variant_registry(
            graphics::pipeline_cache& cache,
            const graphics::root_signature_registry& root_signatures,
            const graphics::shader_library& shaders);

        // Builds the base sets, creating their distinct PSOs in a bulk
        result<void> create_base_sets();

        // One PSO: the shaders and state of (kind, key, mask), matched to a root signature
        result<pipeline_slot> create_slot(pass_kind kind, const pipeline_set_key& key, std::uint8_t mask) const;

        // Every slot of a set, serially
        result<pipeline_set> create_set(const pipeline_set_key& key, bool base) const;

        std::uint32_t append(pipeline_set set);

        graphics::pipeline_cache& cache_;
        const graphics::root_signature_registry& root_signatures_;
        const graphics::shader_library& shaders_;

        std::deque<pipeline_set> sets_; // stable references
        std::unordered_map<pipeline_set_key, std::uint32_t, pipeline_set_key_hash> indices_;
        std::unordered_set<rhi::graphics_pipeline_handle> pipelines_; // distinct PSOs of the slots
        std::size_t base_sets_ = 0;

        mutable std::mutex write_masks_mutex_;
        std::unordered_map<std::uint64_t, pipeline_slot> write_masks_; // (set, kind, mask) → slot
    };
} // namespace avis::visualizer::rendering

#endif
