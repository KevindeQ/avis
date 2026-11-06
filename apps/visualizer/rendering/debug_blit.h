#ifndef AVIS_VISUALIZER_RENDERING_DEBUG_BLIT_H
#define AVIS_VISUALIZER_RENDERING_DEBUG_BLIT_H

// The debug blit and the pixel inspector (docs/design/11-renderer.md §11, docs/tasks/11-renderer.md §11.20): what the
// renderer works out on the CPU. A view's debug_selection names one of its resources; the blit draws it into a
// rectangle of ldr_color (the whole view, or a picture-in-picture corner) after the overlays, and the inspector reads
// back the texel under a cursor in that rectangle (debug_blit.hlsl, passes/debug_blit_pass.h).

#include "rendering/view.h"

#include "avis/graphics/texture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    // A rectangle of the view, pixels from the top-left corner
    struct view_rectangle
    {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        friend bool operator==(const view_rectangle&, const view_rectangle&) = default;
    };

    // Where a selection is drawn: the whole view, or its picture-in-picture corner, `scale` of the view's size
    // (rounded, at least 1 pixel, at most the view)
    view_rectangle debug_blit_rectangle(const debug_selection& selection, graphics::texture_extent view_size) noexcept;

    // The resource of the view `view_name` that a selection's name refers to: the name itself, or the part after
    // "<view_name>/". None when it is qualified with another view.
    std::optional<std::string_view> resource_in_view(std::string_view name, std::string_view view_name) noexcept;

    // The texel of a mip of `source_size` under the cursor (x, y), as the blit maps `rectangle` onto the whole mip;
    // none outside the rectangle
    struct texel_position
    {
        std::uint32_t x = 0;
        std::uint32_t y = 0;

        friend bool operator==(const texel_position&, const texel_position&) = default;
    };

    std::optional<texel_position> inspected_texel(
        const view_rectangle& rectangle,
        graphics::texture_extent source_size,
        std::uint32_t x,
        std::uint32_t y) noexcept;

    // What the inspector read, decoded from its debug_inspector_bytes (debug_blit.hlsl): for a float format, the texel
    // as the shader reads it (missing channels 0, alpha 1) and its bits; for an integer format, the texel as float and
    // the texel; for a depth format, value[0] the stored depth and value[1] its linear depth (metres); for
    // cluster_lights, value[0] the pixel's linear depth, and bits the cluster's offset, count, dropped lights and
    // index.
    struct inspected_value
    {
        texel_position texel;
        std::array<float, 4> value{};
        std::array<std::uint32_t, 4> bits{};
    };

    // Decodes the inspector's bytes; none when there are fewer than debug_inspector_bytes
    std::optional<inspected_value> decode_inspected_value(std::span<const std::byte> bytes, texel_position texel);

    // The result of a view's inspector request
    struct inspector_result
    {
        std::string view;
        inspector_request request;
        std::string resource; // the selection's resource, qualified with the view ("main/linear_depth")
        std::uint32_t mip = 0;
        std::uint32_t slice = 0;
        std::uint64_t frame_index = 0; // of the frame that read it
        // None: the cursor is outside the blit's rectangle, or the selection can't be shown this frame (the warning
        // says why)
        std::optional<inspected_value> value;
    };
} // namespace avis::visualizer::rendering

#endif
