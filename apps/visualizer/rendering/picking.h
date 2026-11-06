#ifndef AVIS_VISUALIZER_RENDERING_PICKING_H
#define AVIS_VISUALIZER_RENDERING_PICKING_H

// Picking (docs/design/11-renderer.md §5.1, docs/tasks/11-renderer.md §11.19). On a frame where a view has a
// pick_request, the renderer draws the camera's pickable instances again into picking_id (R32G32_UINT, cleared to 0),
// after the overlays: the world and overlay groups with each set's picking PSO, then overlay_on_top with its
// picking_on_top PSO (picking.hlsl). A compute pass gathers the pick_window_size² texels around the click into a small
// buffer (pick_window.hlsl), and a copy pass copies it into a readback buffer from the renderer's ring. Once the
// frame's submissions are complete, take_pick_results() reports the window's choice: the non-zero id closest to the
// click (select_pick), which makes thin lines and small points clickable.

#include "rendering/view.h"

#include "avis/scene/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "shared_types.hlsli"

namespace avis::visualizer::rendering
{
    inline constexpr std::uint32_t pick_window_texels = gpu::pick_window_size * gpu::pick_window_size;
    inline constexpr std::uint64_t pick_window_bytes = std::uint64_t{ pick_window_texels } * 8;

    // The window's top-left texel for a click: the click minus half the window; texels outside the view read 0
    struct pick_window_origin
    {
        std::int32_t x = 0;
        std::int32_t y = 0;
    };

    pick_window_origin pick_origin(const pick_request& request) noexcept;

    // An object id under the click, and the texel it was found at (view pixels from the top-left)
    struct pick_hit
    {
        scene::object_id object_id = 0;
        std::uint32_t x = 0;
        std::uint32_t y = 0;

        friend bool operator==(const pick_hit&, const pick_hit&) = default;
    };

    // The non-zero id of `window` (pick_window_texels R32G32_UINT texels, row by row from `origin`) closest to the
    // window's centre; equal distances go to the first in row order. None when every texel is 0.
    std::optional<pick_hit> select_pick(std::span<const std::byte> window, pick_window_origin origin);

    // What a view's pick request found
    struct pick_result
    {
        std::string view;
        pick_request request;
        std::uint64_t frame_index = 0; // of the frame that drew it
        std::optional<pick_hit> hit; // none: nothing pickable under the window
    };
} // namespace avis::visualizer::rendering

#endif
