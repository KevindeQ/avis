#ifndef RUNTIME_GRAPHICS_ENUM_MAPPINGS_H
#define RUNTIME_GRAPHICS_ENUM_MAPPINGS_H

#include "avis/core/common.h"
#include "avis/core/enum_flags.h"

namespace graphics
{
    // Implementation of root_signature_builder assumes that filter exactly matches the implementation of D3D12_FILTER.
    // This includes the values for the different choices matcning the D3D12 equivalent.
    enum class filter
    {
        min_mag_mip_point = 0,
        min_mag_point_mip_linear = 0x1,
        min_point_mag_linear_mip_point = 0x4,
        min_point_mag_mip_linear = 0x5,
        min_linear_mag_mip_point = 0x10,
        min_linear_mag_point_mip_linear = 0x11,
        min_mag_linear_mip_point = 0x14,
        min_mag_mip_linear = 0x15,
        min_mag_anisotropic_mip_point,
        anisotropic = 0x55,
        comparison_min_mag_mip_point = 0x80,
        comparison_min_mag_point_mip_linear = 0x81,
        comparison_min_point_mag_linear_mip_point = 0x84,
        comparison_min_point_mag_mip_linear = 0x85,
        comparison_min_linear_mag_mip_point = 0x90,
        comparison_min_linear_mag_point_mip_linear = 0x91,
        comparison_min_mag_linear_mip_point = 0x94,
        comparison_min_mag_mip_linear = 0x95,
        comparison_min_mag_anisotropic_mip_point,
        comparison_anisotropic = 0xd5,
        minimum_min_mag_mip_point = 0x100,
        minimum_min_mag_point_mip_linear = 0x101,
        minimum_min_point_mag_linear_mip_point = 0x104,
        minimum_min_point_mag_mip_linear = 0x105,
        minimum_min_linear_mag_mip_point = 0x110,
        minimum_min_linear_mag_point_mip_linear = 0x111,
        minimum_min_mag_linear_mip_point = 0x114,
        minimum_min_mag_mip_linear = 0x115,
        minimum_min_mag_anisotropic_mip_point,
        minimum_anisotropic = 0x155,
        maximum_min_mag_mip_point = 0x180,
        maximum_min_mag_point_mip_linear = 0x181,
        maximum_min_point_mag_linear_mip_point = 0x184,
        maximum_min_point_mag_mip_linear = 0x185,
        maximum_min_linear_mag_mip_point = 0x190,
        maximum_min_linear_mag_point_mip_linear = 0x191,
        maximum_min_mag_linear_mip_point = 0x194,
        maximum_min_mag_mip_linear = 0x195,
        maximum_min_mag_anisotropic_mip_point,
        maximum_anisotropic = 0x1d5
    };

    // Implementation of root_signature_builder assumes that texture_address_mode exactly matches the implementation of
    // D3D12_TEXTURE_ADDRESS_MODE. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class texture_address_mode
    {
        wrap = 1,
        mirror = 2,
        clamp = 3,
        border = 4,
        mirror_once = 5
    };

    // Implementation of root_signature_builder assumes that texture_address_mode exactly matches the implementation of
    // D3D12_STATIC_BORDER_COLOR. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class static_border_color
    {
        transparent_black = 0,
        opaque_black,
        opaque_white,
        opaque_black_uint,
        opaque_white_uint
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_SHADER_VISIBILITY. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class shader_visibility
    {
        all = 0,
        vertex = 1,
        hull = 2,
        domain = 3,
        geometry = 4,
        pixel = 5,
        amplification = 6,
        mesh = 7
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_ROOT_SIGNATURE_FLAGS. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class root_signature_flags : std::uint32_t
    {
        none = 0,
        allow_input_assembler_input_layout = 0x1,
        deny_vertex_shader_root_access = 0x2,
        deny_hull_shader_root_access = 0x4,
        deny_domain_shader_root_access = 0x8,
        deny_geometry_shader_root_access = 0x10,
        deny_pixel_shader_root_access = 0x20,
        allow_stream_output = 0x40,
        local_root_signature = 0x80,
        deny_amplification_shader_root_access = 0x100,
        deny_mesh_shader_root_access = 0x200,
        cbv_srv_uav_heap_directly_indexed = 0x400,
        sampler_heap_directly_indexed = 0x800
    };
    DECLARE_ENUM_FLAGS(root_signature_flags);

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_FILL_MODE. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class fill_mode
    {
        wireframe = 2,
        solid = 3
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_CULL_MODE. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class cull_mode
    {
        none = 1,
        front = 2,
        back = 3
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_CONSERVATIVE_RASTERIZATION_MODE. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class conservative_rasterization_mode
    {
        off = 0,
        on = 1
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_DEPTH_WRITE_MASK. This includes the values for the different choices matcning the D3D12
    // equivalent.
    enum class depth_write_mask
    {
        zero = 0,
        all = 1
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_COMPARISON_FUNC. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class comparison_function
    {
        none,
        never = 1,
        less = 2,
        equal = 3,
        less_equal = 4,
        greater = 5,
        greater_equal = 7,
        not_equal = 6,
        always = 8
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_STENCIL_OP. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class depth_stencil_operation
    {
        keep = 1,
        zero = 2,
        replace = 3,
        increment_and_clamp = 4,
        decrement_and_clamp = 5,
        invert = 6,
        increment = 7,
        decrement = 8
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_INDEX_BUFFER_STRIP_CUT_VALUE. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class index_buffer_strip_cut_value
    {
        disabled = 0,
        /*0xFFFF = 1,
        0xFFFFFFFF = 2*/
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_PRIMITIVE_TOPOLOGY_TYPE. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class primitive_topology_type
    {
        undefined = 0,
        point = 1,
        line = 2,
        triangle = 3,
        patch = 4
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // DXGI_FORMAT. This includes the values for the different choices matcning the DXGI equivalent.
    enum class format_descriptor
    {
        unknown = 0,
        r32g32b32a32_typeless = 1,
        r32g32b32a32_float = 2,
        r32g32b32a32_uint = 3,
        r32g32b32a32_sint = 4,
        r32g32b32_typeless = 5,
        r32g32b32_float = 6,
        r32g32b32_uint = 7,
        r32g32b32_sint = 8,
        r16g16b16a16_typeless = 9,
        r16g16b16a16_float = 10,
        r16g16b16a16_unorm = 11,
        r16g16b16a16_uint = 12,
        r16g16b16a16_snorm = 13,
        r16g16b16a16_sint = 14,
        r32g32_typeless = 15,
        r32g32_float = 16,
        r32g32_uint = 17,
        r32g32_sint = 18,
        r32g8x24_typeless = 19,
        d32_float_s8x24_uint = 20,
        r32_float_x8x24_typeless = 21,
        x32_typeless_g8x24_uint = 22,
        r10g10b10a2_typeless = 23,
        r10g10b10a2_unorm = 24,
        r10g10b10a2_uint = 25,
        r11g11b10_float = 26,
        r8g8b8a8_typeless = 27,
        r8g8b8a8_unorm = 28,
        r8g8b8a8_unorm_srgb = 29,
        r8g8b8a8_uint = 30,
        r8g8b8a8_snorm = 31,
        r8g8b8a8_sint = 32,
        r16g16_typeless = 33,
        r16g16_float = 34,
        r16g16_unorm = 35,
        r16g16_uint = 36,
        r16g16_snorm = 37,
        r16g16_sint = 38,
        r32_typeless = 39,
        d32_float = 40,
        r32_float = 41,
        r32_uint = 42,
        r32_sint = 43,
        r24g8_typeless = 44,
        d24_unorm_s8_uint = 45,
        r24_unorm_x8_typeless = 46,
        x24_typeless_g8_uint = 47,
        r8g8_typeless = 48,
        r8g8_unorm = 49,
        r8g8_uint = 50,
        r8g8_snorm = 51,
        r8g8_sint = 52,
        r16_typeless = 53,
        r16_float = 54,
        d16_unorm = 55,
        r16_unorm = 56,
        r16_uint = 57,
        r16_snorm = 58,
        r16_sint = 59,
        r8_typeless = 60,
        r8_unorm = 61,
        r8_uint = 62,
        r8_snorm = 63,
        r8_sint = 64,
        a8_unorm = 65,
        r1_unorm = 66,
        r9g9b9e5_sharedexp = 67,
        r8g8_b8g8_unorm = 68,
        g8r8_g8b8_unorm = 69,
        bc1_typeless = 70,
        bc1_unorm = 71,
        bc1_unorm_srgb = 72,
        bc2_typeless = 73,
        bc2_unorm = 74,
        bc2_unorm_srgb = 75,
        bc3_typeless = 76,
        bc3_unorm = 77,
        bc3_unorm_srgb = 78,
        bc4_typeless = 79,
        bc4_unorm = 80,
        bc4_snorm = 81,
        bc5_typeless = 82,
        bc5_unorm = 83,
        bc5_snorm = 84,
        b5g6r5_unorm = 85,
        b5g5r5a1_unorm = 86,
        b8g8r8a8_unorm = 87,
        b8g8r8x8_unorm = 88,
        r10g10b10_xr_bias_a2_unorm = 89,
        b8g8r8a8_typeless = 90,
        b8g8r8a8_unorm_srgb = 91,
        b8g8r8x8_typeless = 92,
        b8g8r8x8_unorm_srgb = 93,
        bc6h_typeless = 94,
        bc6h_uf16 = 95,
        bc6h_sf16 = 96,
        bc7_typeless = 97,
        bc7_unorm = 98,
        bc7_unorm_srgb = 99
    };

    // Implementation of root_signature_builder assumes that root_signature_flags exactly matches the implementation of
    // D3D12_PIPELINE_STATE_FLAGS. This includes the values for the different choices matcning the D3D12 equivalent.
    enum class pipeline_state_flags
    {
        none = 0,
        tool_debug = 1,
        dynamic_depth_bias,
        dynamic_index_buffer_strip_cut
    };
}

#endif
