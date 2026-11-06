#include "rendering/debug_blit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace avis::visualizer::rendering
{
    view_rectangle
        debug_blit_rectangle(const debug_selection& selection, const graphics::texture_extent view_size) noexcept
    {
        if (!selection.picture_in_picture)
        {
            return { .x = 0, .y = 0, .width = view_size.width, .height = view_size.height };
        }
        const float scale = std::clamp(selection.picture_in_picture->scale, 0.0f, 1.0f);
        const auto scaled = [scale](const std::uint32_t extent)
        {
            const auto rounded = static_cast<std::uint32_t>(std::lround(static_cast<float>(extent) * scale));
            return std::clamp(rounded, 1u, extent);
        };
        const std::uint32_t width = scaled(view_size.width);
        const std::uint32_t height = scaled(view_size.height);
        const view_corner corner = selection.picture_in_picture->corner;
        const bool right = corner == view_corner::top_right || corner == view_corner::bottom_right;
        const bool bottom = corner == view_corner::bottom_left || corner == view_corner::bottom_right;
        return { .x = right ? view_size.width - width : 0,
                 .y = bottom ? view_size.height - height : 0,
                 .width = width,
                 .height = height };
    }

    std::optional<std::string_view>
        resource_in_view(const std::string_view name, const std::string_view view_name) noexcept
    {
        const std::size_t separator = name.find('/');
        if (separator == std::string_view::npos)
        {
            return name;
        }
        if (name.substr(0, separator) != view_name)
        {
            return std::nullopt;
        }
        return name.substr(separator + 1);
    }

    std::optional<texel_position> inspected_texel(
        const view_rectangle& rectangle,
        const graphics::texture_extent source_size,
        const std::uint32_t x,
        const std::uint32_t y) noexcept
    {
        if (x < rectangle.x || y < rectangle.y || x - rectangle.x >= rectangle.width ||
            y - rectangle.y >= rectangle.height || source_size.width == 0 || source_size.height == 0)
        {
            return std::nullopt;
        }
        // The pixel's centre, as SV_Position gives it to the blit's pixel shader
        const auto map = [](const std::uint32_t pixel, const std::uint32_t extent, const std::uint32_t source)
        {
            const double local = (static_cast<double>(pixel) + 0.5) / static_cast<double>(extent);
            return std::min(static_cast<std::uint32_t>(local * static_cast<double>(source)), source - 1);
        };
        return texel_position{ .x = map(x - rectangle.x, rectangle.width, source_size.width),
                               .y = map(y - rectangle.y, rectangle.height, source_size.height) };
    }

    std::optional<inspected_value>
        decode_inspected_value(const std::span<const std::byte> bytes, const texel_position texel)
    {
        if (bytes.size() < gpu::debug_inspector_bytes)
        {
            return std::nullopt;
        }
        inspected_value decoded{ .texel = texel };
        std::memcpy(decoded.value.data(), bytes.data(), sizeof(decoded.value));
        std::memcpy(decoded.bits.data(), bytes.data() + sizeof(decoded.value), sizeof(decoded.bits));
        return decoded;
    }
} // namespace avis::visualizer::rendering
