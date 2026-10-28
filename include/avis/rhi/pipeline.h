#ifndef AVIS_RHI_PIPELINE_H
#define AVIS_RHI_PIPELINE_H

// Pipeline descriptions (docs/design/09-rhi.md §12). The pipelines view follows with the pipeline factory.

#include "avis/rhi/formats.h"
#include "avis/rhi/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace avis::rhi
{
    enum class cull_mode : std::uint8_t
    {
        none,
        front,
        back
    };

    enum class fill_mode : std::uint8_t
    {
        solid,
        wireframe
    };

    enum class blend_factor : std::uint8_t
    {
        zero,
        one,
        source_alpha,
        inverse_source_alpha,
        destination_alpha,
        inverse_destination_alpha,
        source_color,
        inverse_source_color
    };

    enum class blend_operation : std::uint8_t
    {
        add,
        subtract,
        reverse_subtract,
        minimum,
        maximum
    };

    enum class primitive_topology : std::uint8_t
    {
        triangle_list,
        triangle_strip,
        line_list,
        line_strip,
        point_list
    };

    struct rasterizer_state
    {
        cull_mode cull = cull_mode::back;
        fill_mode fill = fill_mode::solid;
        bool front_counter_clockwise = true; // glTF convention
        std::int32_t depth_bias = 0;
        float slope_scaled_depth_bias = 0.0f;
        float depth_bias_clamp = 0.0f;
        bool depth_clip = true;
    };

    struct depth_state
    {
        bool test = true;
        bool write = true;
        comparison compare = comparison::greater_equal; // reverse-Z
    };

    struct blend_state
    {
        bool enable = false;
        blend_factor source_color = blend_factor::one;
        blend_factor destination_color = blend_factor::zero;
        blend_operation color_operation = blend_operation::add;
        blend_factor source_alpha = blend_factor::one;
        blend_factor destination_alpha = blend_factor::zero;
        blend_operation alpha_operation = blend_operation::add;
        std::uint8_t write_mask = 0xF;
    };

    // Everything of a graphics pipeline except its shaders; shared with the graphics layer's descriptions (18 §11)
    struct graphics_pipeline_state
    {
        rasterizer_state rasterizer;
        depth_state depth;
        std::array<blend_state, 8> blend{};
        std::uint32_t render_target_count = 0;
        std::array<format, 8> render_target_formats{};
        format depth_format = format::unknown;
        primitive_topology topology = primitive_topology::triangle_list;
    };

    struct graphics_pipeline_description
    {
        root_signature_handle root_signature; // required (§12.1)
        std::span<const std::byte> vertex_shader; // DXIL; only read during create_pipeline
        std::span<const std::byte> pixel_shader; // empty = depth-only
        graphics_pipeline_state state;
        std::string_view debug_name; // copied at creation
    };

    struct compute_pipeline_description
    {
        root_signature_handle root_signature; // required
        std::span<const std::byte> compute_shader; // DXIL; only read during create_pipeline
        std::string_view debug_name; // copied at creation
    };
} // namespace avis::rhi

#endif
