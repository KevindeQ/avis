#ifndef AVIS_VISUALIZER_RENDERING_RANGE_WALKER_H
#define AVIS_VISUALIZER_RENDERING_RANGE_WALKER_H

// The range walker (docs/design/11-renderer.md §4.6–§4.8, docs/tasks/11-renderer.md §11.9): records a key range of a
// view's draw list into a pass's recorder.
//
// Prepare stage, serially, once per draw list: prepare_draw_runs cuts the whole list into runs, and writes each draw
// run's instance indices contiguously into the frame's instance_indices array (pass 0 uploads it). A run is one
// command item, or the draw items with one key whose records share a mesh and submesh: one instanced draw. Progressive
// meshes draw one instance per run (11 §9.3). Translucent groups merge only identical keys, which every run does. The
// offsets are fixed before recording, so parts of a range recorded in parallel don't race.
//
// Recording, on any worker: walk records the runs of a range in key order.
//   - Command items run through a command_context. clear_* commands check the attachment against the pass's declared
//     writes (pass_bindings), a debug check.
//   - Each draw run pushes its scene_draw_constants with binding_layout::push_draw_constants and calls draw_indexed or
//     draw. Lines and points draw six vertices per segment or point (11 §9).
//   - The redundant-state filter remembers the pipeline, its root signature, the draw constants, the pass and view
//     constants, the viewport and the scissor, and only records changes. A root signature switch forgets the root
//     arguments (18 §10.3), so they are set again. The index buffer is bound once per walk.
//   - A new value of the key's viewport field binds that viewport's targets (a cascade's slice of the shadow map), its
//     viewport, scissor and view constants.
//   - One PIX event per group, named from its prefix ("scene/camera/world/opaque").
//
// Recording groups (10 §7): a long range is recorded in several parts, split at run boundaries (split_runs). Each part
// is its own graph pass, which the graph puts into recording groups recorded in parallel; each walk starts with a clean
// filter.

#include "rendering/draw_lists.h"
#include "rendering/pipeline_variant_registry.h"
#include "rendering/sort_key.h"

#include "avis/graphics/pass_context.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace avis::graphics
{
    class linear_arena;
} // namespace avis::graphics

namespace avis::visualizer::rendering
{
    // A run of a draw list: one command item, or the draw items of one instanced draw
    struct draw_run
    {
        std::uint32_t first = 0; // index of its first item in draw_list::items()
        std::uint32_t count = 0; // items: the draw's instances, or 1 for a command
        std::uint32_t instance_offset = 0; // the draw's first entry in the frame's instance_indices; 0 for a command

        friend bool operator==(const draw_run&, const draw_run&) = default;
    };

    // A draw list with its runs, which cover its items in order
    struct prepared_draw_list
    {
        const draw_list* list = nullptr;
        std::span<const draw_run> runs; // in the arena the list is in

        // The runs of `items`, a range of list->items() (draw_list::range). Ranges start and end at group boundaries,
        // which are run boundaries.
        std::span<const draw_run> runs_of(std::span<const draw_item> items) const noexcept;
    };

    // Cuts `list` into runs and appends each draw run's instance indices (draw_record::instance) to `instance_indices`.
    // `meshes` is the frame's mesh table the list was built with. Prepare stage only.
    prepared_draw_list prepare_draw_runs(
        const draw_list& list,
        std::span<const draw_mesh> meshes,
        graphics::linear_arena& arena,
        std::vector<std::uint32_t>& instance_indices);

    // ---- commands (11 §4.7) ----

    // The arguments of clear_depth_stencil
    namespace clear_flags
    {
        inline constexpr std::uint32_t depth = 1u << 0;
        inline constexpr std::uint32_t stencil = 1u << 1; // not supported: the depth formats have no stencil (11.9)
    } // namespace clear_flags

    struct clear_depth_stencil_arguments
    {
        float depth = 0.0f; // reverse-Z: 0 is the far plane
        std::uint32_t stencil = 0;
        std::uint32_t flags = clear_flags::depth;
    };

    struct clear_render_target_arguments
    {
        std::uint32_t attachment = 0; // index into the pass's colour attachments (viewport_binding::render_targets)
        std::array<float, 4> color{};
    };

    struct viewport_scissor_arguments
    {
        rhi::viewport viewport;
        rhi::rectangle scissor;
    };

    // Builds the renderer's command items and their arguments for draw_list_sources::commands and command_arguments.
    // Every item gets its own argument offset, so values are unique. Arguments are 16-byte aligned.
    class draw_commands
    {
    public:
        // Clears the depth attachment of the group's viewport (the pass must write depth)
        void clear_depth_stencil(
            key_group group, std::uint32_t sequence, const clear_depth_stencil_arguments& arguments);
        // Clears one colour attachment of the group's viewport
        void clear_render_target(
            key_group group, std::uint32_t sequence, const clear_render_target_arguments& arguments);
        // Overrides the viewport and scissor for the rest of the walk, or until reset_state
        void set_viewport_scissor(key_group group, std::uint32_t sequence, const viewport_scissor_arguments& arguments);
        // Not supported in iteration 1 (no stencil): fails a debug check when walked
        void set_stencil_reference(key_group group, std::uint32_t sequence, std::uint8_t reference);
        // The following draws use write_mask_variant(set, kind, mask) (write_mask)
        void set_write_mask(key_group group, std::uint32_t sequence, std::uint8_t mask);
        // Drops the viewport, scissor and write-mask overrides
        void reset_state(key_group group, std::uint32_t sequence);

        // Calls `function` with a copy of `arguments`; the walker forgets its remembered state afterwards
        void callback(
            key_group group, std::uint32_t sequence, command_callback function, std::span<const std::byte> arguments);

        template<typename Arguments>
            requires std::is_trivially_copyable_v<Arguments>
        void callback(
            const key_group group,
            const std::uint32_t sequence,
            const command_callback function,
            const Arguments& arguments)
        {
            callback(group, sequence, function, std::as_bytes(std::span{ &arguments, 1 }));
        }

        std::span<const draw_item> items() const noexcept
        {
            return items_;
        }

        std::span<const std::byte> arguments() const noexcept
        {
            return arguments_;
        }

        void clear() noexcept
        {
            items_.clear();
            arguments_.clear();
        }

    private:
        // The offset of `bytes` (at least 4 bytes, so every item has its own), appended at a 16-byte boundary
        std::uint32_t append(std::span<const std::byte> bytes);
        void add(const command_key_fields& fields, std::span<const std::byte> arguments);

        std::vector<draw_item> items_;
        std::vector<std::byte> arguments_;
    };

    // ---- what a pass binds ----

    // One viewport of the key, as a pass draws it: the camera's targets, or a cascade's slice of the shadow map
    struct viewport_binding
    {
        std::span<const rhi::texture_view> render_targets; // the pass's colour attachments, in its PSOs' order
        std::optional<rhi::texture_view> depth;
        rhi::viewport viewport;
        rhi::rectangle scissor;
        rhi::gpu_address view_constants{}; // b2, view_constants of this viewpoint
    };

    struct pass_bindings
    {
        // By the key's viewport index (0 the camera, 1–4 the cascades). Items of a viewport without a binding fail a
        // debug check and aren't recorded.
        std::array<std::optional<viewport_binding>, 8> viewports;
        bool depth_written = false; // the pass declared a depth write: clear_depth_stencil may clear it
        rhi::gpu_address pass_constants{}; // b1, scene_pass_constants
    };

    // What every walk of a frame reads; shared by the parts recorded in parallel, which only read it
    struct range_walk_sources
    {
        const prepared_draw_list& list;
        std::span<const draw_mesh> meshes; // the frame's mesh table the list was built with
        pipeline_variant_registry& pipelines; // set() and the thread-safe write_mask_variant only
        rhi::buffer_handle index_buffer; // the geometry arena's
    };

    // What a walk recorded: the redundant-state filter's counters among them (11 §15)
    struct range_walk_statistics
    {
        std::size_t runs = 0;
        std::size_t draws = 0;
        std::size_t instances = 0;
        std::size_t commands = 0;
        std::size_t events = 0; // PIX groups
        std::size_t pipeline_changes = 0; // set_pipeline calls
        std::size_t signature_changes = 0; // of which switched the root signature
        std::size_t constant_pushes = 0; // push_draw_constants calls
        std::size_t viewport_changes = 0; // new values of the key's viewport field
        std::size_t viewport_sets = 0; // set_viewport calls
        std::size_t scissor_sets = 0; // set_scissor calls
        std::size_t skipped_not_drawn = 0; // items whose set has no PSO for the pass kind
        std::size_t skipped_invalid = 0; // items of an unbound viewport, a bad submesh, or failed commands

        range_walk_statistics& operator+=(const range_walk_statistics& other) noexcept;
    };

    // A command's view of the walk (11 §4.7). Callbacks may record anything through the recorder; the walker forgets
    // its remembered state after them.
    class command_context
    {
    public:
        command_context(
            graphics::graphics_pass_context& pass, const viewport_binding& targets, const key_group group) noexcept :
            pass_{ &pass },
            targets_{ &targets },
            group_{ group }
        {}

        graphics::graphics_pass_context& pass() noexcept
        {
            return *pass_;
        }

        rhi::graphics_recorder& recorder() noexcept
        {
            return pass_->recorder();
        }

        // The targets of the command's viewport
        const viewport_binding& targets() const noexcept
        {
            return *targets_;
        }

        key_group group() const noexcept
        {
            return group_;
        }

    private:
        graphics::graphics_pass_context* pass_;
        const viewport_binding* targets_;
        key_group group_;
    };

    // Records `runs` (from prepared_draw_list::runs_of, or a part of them) with each set's `kind` PSO. Starts with a
    // clean filter. Thread-safe for distinct recorders.
    range_walk_statistics walk(
        std::span<const draw_run> runs,
        pass_kind kind,
        graphics::graphics_pass_context& context,
        const range_walk_sources& sources,
        const pass_bindings& bindings);

    // "scene/camera/world/opaque", "scene/shadow_cascade[2]/world/opaque": the PIX event of a group
    std::string group_name(const key_group& group);

    // ---- recording groups (10 §7) ----

    struct recording_split
    {
        std::uint32_t items_per_part = 4096; // the target item count of a part
        std::uint32_t passes_per_group = 4; // render_graph_configuration::passes_per_group_without_timings
        std::uint32_t maximum_parts = 16;
    };

    // How many parts a range of `items` items is recorded in: one, unless it is longer than passes_per_group ×
    // items_per_part (fewer parts would land in one recording group anyway); then about items_per_part items per part,
    // at most maximum_parts
    std::uint32_t recording_part_count(std::size_t items, const recording_split& split) noexcept;

    // `runs` in at most `parts` contiguous, non-empty parts with about equal item counts, split at run boundaries
    std::vector<std::span<const draw_run>> split_runs(std::span<const draw_run> runs, std::uint32_t parts);

    // The items of `runs`
    std::size_t item_count(std::span<const draw_run> runs) noexcept;
} // namespace avis::visualizer::rendering

#endif
