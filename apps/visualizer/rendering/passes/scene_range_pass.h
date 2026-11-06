#ifndef AVIS_VISUALIZER_RENDERING_PASSES_SCENE_RANGE_PASS_H
#define AVIS_VISUALIZER_RENDERING_PASSES_SCENE_RANGE_PASS_H

// The graphics passes that walk a key range of a view's draw list (docs/design/11-renderer.md §4.6, §4.8,
// docs/tasks/11-renderer.md §11.9, §11.10): the depth prepass, the G-buffer and, later, the shadow, forward and overlay
// passes.
//
// A range is recorded in one graph pass per recording part (split_runs): "<name>", then "<name>_part1", … Every part
// writes the same attachments, so the graph orders them and puts them into recording groups recorded in parallel. The
// first part creates the attachments that don't exist yet, and records the clears asked for (every layer, to the
// description's clear value), since the graph's write_flags::needs_clear only declares the need. Every part reads the
// frame's instances and instance indices, the geometry arena (the index buffer as both index buffer and SRV, for line
// meshes) and the material table, and binds them as scene_pass_constants (b1). Each viewport of the key the pass draws
// is bound to its layer of the attachments with its own view constants (b2): by default the camera, with the view's.
// A pass can read more resources in its pixel shader (scene_pass_reads) and append its own constants after
// scene_pass_constants in b1 (scene_range_pass::extension), as the forward pass does with its lighting inputs.

#include "rendering/frame_data.h"
#include "rendering/pipeline_variant_registry.h"
#include "rendering/range_walker.h"
#include "rendering/sort_key.h"

#include "avis/graphics/render_graph.h"
#include "avis/graphics/texture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    // Resources a pass reads in its pixel shader, beyond what every scene pass reads; invalid entries are skipped
    struct scene_pass_reads
    {
        std::array<graphics::texture, 8> textures{};
        std::array<graphics::buffer, 8> buffers{};
    };

    // The SRVs of a pass's scene_pass_reads, by the same positions; 0 for an invalid entry
    struct scene_pass_read_srvs
    {
        std::array<std::uint32_t, 8> textures{};
        std::array<std::uint32_t, 8> buffers{};
    };

    // The bytes a pass appends after scene_pass_constants in b1, built when recording from its reads' SRVs. It's
    // called once per part, possibly on several threads at once.
    using scene_pass_extension = std::function<std::vector<std::byte>(const scene_pass_read_srvs&)>;

    // One viewport of the key a pass draws, bound to one layer of its attachments
    struct scene_viewport
    {
        rendering::viewport point;
        std::uint32_t layer = 0; // the attachments' array slice
        gpu::view_constants view_constants;
    };

    struct scene_range_pass
    {
        std::string_view name;
        pass_kind kind = pass_kind::depth;
        key_range range;
        float line_margin = 0.0f; // scene_pass_constants::line_margin
        std::span<const scene_viewport> viewports; // empty: the camera, layer 0, with inputs.view_constants
        scene_pass_reads reads; // read as srv_pixel by every part
        scene_pass_extension extension; // empty: b1 is scene_pass_constants alone
        // Runs to walk instead of the range's: a subset of the list's runs in key order, valid until the graph is
        // reset (the selection mask's, 11.17)
        std::optional<std::span<const draw_run>> runs;
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
