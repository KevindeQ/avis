#ifndef AVIS_RHI_PIPELINE_H
#define AVIS_RHI_PIPELINE_H

// Pipeline descriptions and the pipelines view (docs/design/09-rhi.md §4.3, §12). Free of D3D12 and <windows.h>.

#include "avis/error.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/root_signature.h"
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

    class render_hardware_interface;

    namespace d3d12
    {
        class device_state;
    } // namespace d3d12

    // A view (§4.3): non-owning, copyable, built from the RHI and never outliving it. Any thread. Every call creates
    // a native object (no cache, §12); root signatures and pipelines live as long as the RHI (D-87).
    class pipelines
    {
    public:
        explicit pipelines(render_hardware_interface& hardware_interface) noexcept;

        // invalid_argument naming the parameter (§12.1); native_error with the serialiser's message
        result<root_signature_handle> create_root_signature(const root_signature_description& description);

        // invalid_handle for a stale root signature, invalid_argument for an empty shader or bad formats,
        // pipeline_creation_failed (debug name and HRESULT) when D3D12 rejects it
        result<graphics_pipeline_handle> create_pipeline(const graphics_pipeline_description& description);
        result<compute_pipeline_handle> create_pipeline(const compute_pipeline_description& description);

    private:
        d3d12::device_state* state_;
    };
} // namespace avis::rhi

#endif
