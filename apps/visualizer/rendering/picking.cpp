#include "rendering/picking.h"

#include <cstring>
#include <limits>

namespace avis::visualizer::rendering
{
    pick_window_origin pick_origin(const pick_request& request) noexcept
    {
        constexpr auto half = static_cast<std::int32_t>(gpu::pick_window_size / 2);
        return { .x = static_cast<std::int32_t>(request.x) - half, .y = static_cast<std::int32_t>(request.y) - half };
    }

    std::optional<pick_hit> select_pick(const std::span<const std::byte> window, const pick_window_origin origin)
    {
        if (window.size() < pick_window_bytes)
        {
            return std::nullopt;
        }
        constexpr auto size = static_cast<std::int32_t>(gpu::pick_window_size);
        constexpr std::int32_t centre = size / 2;
        std::optional<pick_hit> best;
        std::int32_t best_distance = std::numeric_limits<std::int32_t>::max();
        for (std::int32_t row = 0; row < size; ++row)
        {
            for (std::int32_t column = 0; column < size; ++column)
            {
                std::uint32_t words[2];
                std::memcpy(words, window.data() + static_cast<std::size_t>(row * size + column) * 8, sizeof(words));
                const scene::object_id id = (scene::object_id{ words[1] } << 32) | words[0];
                const std::int32_t distance = (row - centre) * (row - centre) + (column - centre) * (column - centre);
                if (id == 0 || distance >= best_distance)
                {
                    continue;
                }
                // Texels outside the view read 0, so a non-zero one is inside it
                best = pick_hit{ .object_id = id,
                                 .x = static_cast<std::uint32_t>(origin.x + column),
                                 .y = static_cast<std::uint32_t>(origin.y + row) };
                best_distance = distance;
            }
        }
        return best;
    }
} // namespace avis::visualizer::rendering
