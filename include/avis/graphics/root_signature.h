#ifndef GRAPHICS_ROOT_SIGNATURE_H
#define GRAPHICS_ROOT_SIGNATURE_H

#include "avis/common.h"
#include "avis/graphics/render_device.h"
#include "avis/utils/enum_flags.h"

namespace graphics
{
    class root_signature
    {
    public:
        root_signature();
        root_signature(const com_ptr<ID3D12RootSignature>& rs);

        com_ptr<ID3D12RootSignature> native_ptr() const;

    private:
        com_ptr<ID3D12RootSignature> root_sig;
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
        opaque_black = 1,
        opaque_white = 2,
        opaque_black_uint = 3,
        opaque_white_uint = 4
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
    ALLOW_FLAGS_FOR_ENUM(root_signature_flags);

    class root_signature_builder
    {
    public:
        root_signature_builder();
        root_signature_builder(const root_signature_builder&) = delete;
        root_signature_builder(root_signature_builder&&) = delete;

        root_signature_builder& operator=(const root_signature_builder&) = delete;
        root_signature_builder& operator=(root_signature_builder&&) = delete;

        root_signature_builder& with_parameter_table_constant_buffer_view(
            std::uint32_t descriptor_count,
            std::uint32_t base_shader_register,
            std::uint32_t register_space,
            std::uint32_t offset_from_table_start,
            shader_visibility shader_vis);
        root_signature_builder& with_parameter_table_shader_resource_view(
            std::uint32_t descriptor_count,
            std::uint32_t base_shader_register,
            std::uint32_t register_space,
            std::uint32_t offset_from_table_start,
            shader_visibility shader_vis);
        root_signature_builder& with_parameter_table_unordered_access_view(
            std::uint32_t descriptor_count,
            std::uint32_t base_shader_register,
            std::uint32_t register_space,
            std::uint32_t offset_from_table_start,
            shader_visibility shader_vis);
        root_signature_builder& with_parameter_table_sampler(
            std::uint32_t descriptor_count,
            std::uint32_t base_shader_register,
            std::uint32_t register_space,
            std::uint32_t offset_from_table_start,
            shader_visibility shader_vis);

        root_signature_builder& with_parameter_constant(
            std::uint32_t shader_register,
            std::uint32_t register_space,
            std::uint32_t constant_count_in_slot,
            shader_visibility shader_vis);

        root_signature_builder& with_parameter_constant_buffer_view(
            std::uint32_t shader_register, std::uint32_t register_space, shader_visibility shader_vis);
        root_signature_builder& with_parameter_shader_resource_view(
            std::uint32_t shader_register, std::uint32_t register_space, shader_visibility shader_vis);
        root_signature_builder& with_parameter_unordered_access_view(
            std::uint32_t shader_register, std::uint32_t register_space, shader_visibility shader_vis);

        root_signature_builder& with_sampler(
            filter filter_method,
            texture_address_mode address_mode_u,
            texture_address_mode address_mode_v,
            texture_address_mode address_mode_w,
            float mip_lod_bias,
            std::uint32_t max_anisotropy,
            comparison_function compare_func,
            static_border_color border_color,
            float min_lod,
            float max_lod,
            std::uint32_t shader_register,
            std::uint32_t register_space,
            shader_visibility shader_vis);

        root_signature_builder& with_flags(const flags<root_signature_flags> flags);

        root_signature build(render_device& device);

        void reset();

    private:
        D3D12_ROOT_SIGNATURE_DESC descriptor;
        std::vector<D3D12_ROOT_PARAMETER> root_parameters;
        std::vector<D3D12_DESCRIPTOR_RANGE> descriptor_ranges;
        std::vector<D3D12_STATIC_SAMPLER_DESC> sampler_descriptors;
    };
} // namespace graphics

#endif
