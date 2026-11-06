#ifndef AVIS_VISUALIZER_RENDERING_PASSES_SCENE_RANGE_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_SCENE_RANGE_PASS_H

// The graphics passes that walk a key range of a view's draw list (docs/design/11-renderer.md §4.6, §4.8,
// docs/tasks/11-renderer.md §11.9, §11.10): the depth prepass, the G-buffer and, later, the shadow, forward and overlay
// passes.
//
// A range is recorded in one graph pass per recording part (split_runs): "<name>", then "<name>_part1", … Every part
// writes the same attachments, so the graph orders them and puts them into recording groups recorded in parallel. The
// first part creates the attachments that don't exist yet, and clears the ones asked for. Every part reads the frame's
// instances and instance indices, the geometry arena (the index buffer as both index buffer and SRV, for line meshes)
// and the material table, and binds them as scene_pass_constants (b1). The camera viewport is bound with the view's
// constants (b2).

#include "rendering/frame_data.h"
#include "rendering/pipeline_variant_registry.h"
#include "rendering/range_walker.h"
#include "rendering/sort_key.h"

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    // What every scene pass of one view reads. Valid until the frame's graph is reset.
    struct scene_pass_inputs
    {
        const prepared_draw_list& list;
        std::span<const draw_mesh> meshes; // the frame's mesh table the list was built with
        pipeline_variant_registry& pipelines;
        const frame_buffers& frame;
        const frame_geometry& geometry;
        const frame_materials& materials;
        gpu::view_constants view_constants; // the camera's
        recording_split split;
    };

    // One attachment of a scene pass: an existing version to write, or a texture the first part creates
    struct scene_attachment
    {
        graphics::texture texture; // invalid: created by the first part from `name` and `description`
        std::string_view name;
        graphics::texture_description description;
        bool clear = false; // the first part clears it to the description's clear value (write_flags::needs_clear)
    };

    struct scene_attachments
    {
        std::array<scene_attachment, 8> colors{};
        std::size_t color_count = 0;
        std::optional<scene_attachment> depth;
    };

    // The versions the last part wrote
    struct scene_pass_outputs
    {
        std::array<graphics::texture, 8> colors{};
        std::size_t color_count = 0;
        graphics::texture depth;
        std::size_t parts = 0;
    };

    struct scene_range_pass
    {
        std::string_view name;
        pass_kind kind = pass_kind::depth;
        key_range range;
        float line_margin = 0.0f; // scene_pass_constants::line_margin
    };

    // Adds the parts of `pass` to the open view of `graph`. The attachments' formats must be attachment_formats(kind).
    // The depth attachment is written (depth_write) by every part, even with PSOs that don't write it: the RHI's depth
    // views are writable, and a writable view needs the depth_write layout.
    scene_pass_outputs add_scene_range_pass(
        graphics::render_graph& graph,
        const scene_range_pass& pass,
        const scene_pass_inputs& inputs,
        const scene_attachments& attachments);
} // namespace avis::visualizer::rendering

#endif
