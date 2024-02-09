#ifndef RUNTIME_GRAPHICS_ROOT_SIGNATURE_H
#define RUNTIME_GRAPHICS_ROOT_SIGNATURE_H

#include "avis/core/common.h"
#include "avis/runtime/graphics/enum_mappings.h"
#include "avis/runtime/graphics/graphics_device.h"

namespace graphics
{
    class root_signature
    {
    public:
        root_signature(const com_ptr<ID3D12RootSignature>& rs);

        com_ptr<ID3D12RootSignature> native_ptr() const;

    private:
        com_ptr<ID3D12RootSignature> root_sig;
    };

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

        root_signature_builder& with_flags(const std::underlying_type_t<root_signature_flags> flags);

        root_signature build(graphics_device& device);

        void reset();

    private:
        D3D12_ROOT_SIGNATURE_DESC descriptor;
        std::vector<D3D12_ROOT_PARAMETER> root_parameters;
        std::vector<D3D12_DESCRIPTOR_RANGE> descriptor_ranges;
        std::vector<D3D12_STATIC_SAMPLER_DESC> sampler_descriptors;
    };
} // namespace graphics

#endif
