#include "avis/runtime/graphics/root_signature.h"

namespace graphics
{
    root_signature::root_signature() : root_sig{} {}

    root_signature::root_signature(const com_ptr<ID3D12RootSignature>& rs) : root_sig{ rs } {}

    com_ptr<ID3D12RootSignature> root_signature::native_ptr() const
    {
        return root_sig;
    }

    root_signature_builder::root_signature_builder() : descriptor{}, descriptor_ranges{}, sampler_descriptors{} {}

    root_signature_builder& root_signature_builder::with_parameter_table_constant_buffer_view(
        std::uint32_t descriptor_count,
        std::uint32_t base_shader_register,
        std::uint32_t register_space,
        std::uint32_t offset_from_table_start,
        shader_visibility shader_vis)
    {
        std::uint32_t range_index = descriptor_ranges.size();
        descriptor_ranges.push_back(
            D3D12_DESCRIPTOR_RANGE{ .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                                    .NumDescriptors = descriptor_count,
                                    .BaseShaderRegister = base_shader_register,
                                    .RegisterSpace = register_space,
                                    .OffsetInDescriptorsFromTableStart = offset_from_table_start });

        // Defer setting the pointer to the descriptor range to the build method because adding a new entry into
        // descriptor_ranges may change the in-memory pointer of the data in descriptor_ranges due to a resize of the
        // std::vector.
        root_parameters.push_back(D3D12_ROOT_PARAMETER{
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE::D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = { .NumDescriptorRanges = range_index, .pDescriptorRanges = nullptr },
            .ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(shader_vis) });

        return *this;
    }

    root_signature_builder& root_signature_builder::with_parameter_table_shader_resource_view(
        std::uint32_t descriptor_count,
        std::uint32_t base_shader_register,
        std::uint32_t register_space,
        std::uint32_t offset_from_table_start,
        shader_visibility shader_vis)
    {
        std::uint32_t range_index = descriptor_ranges.size();
        descriptor_ranges.push_back(
            D3D12_DESCRIPTOR_RANGE{ .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                                    .NumDescriptors = descriptor_count,
                                    .BaseShaderRegister = base_shader_register,
                                    .RegisterSpace = register_space,
                                    .OffsetInDescriptorsFromTableStart = offset_from_table_start });

        // Defer setting the pointer to the descriptor range to the build method because adding a new entry into
        // descriptor_ranges may change the in-memory pointer of the data in descriptor_ranges due to a resize of the
        // std::vector.
        root_parameters.push_back(D3D12_ROOT_PARAMETER{
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE::D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = { .NumDescriptorRanges = range_index, .pDescriptorRanges = nullptr },
            .ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(shader_vis) });

        return *this;
    }

    root_signature_builder& root_signature_builder::with_parameter_table_unordered_access_view(
        std::uint32_t descriptor_count,
        std::uint32_t base_shader_register,
        std::uint32_t register_space,
        std::uint32_t offset_from_table_start,
        shader_visibility shader_vis)
    {
        std::uint32_t range_index = descriptor_ranges.size();
        descriptor_ranges.push_back(
            D3D12_DESCRIPTOR_RANGE{ .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                                    .NumDescriptors = descriptor_count,
                                    .BaseShaderRegister = base_shader_register,
                                    .RegisterSpace = register_space,
                                    .OffsetInDescriptorsFromTableStart = offset_from_table_start });

        // Defer setting the pointer to the descriptor range to the build method because adding a new entry into
        // descriptor_ranges may change the in-memory pointer of the data in descriptor_ranges due to a resize of the
        // std::vector.
        root_parameters.push_back(D3D12_ROOT_PARAMETER{
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE::D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = { .NumDescriptorRanges = range_index, .pDescriptorRanges = nullptr },
            .ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(shader_vis) });

        return *this;
    }

    root_signature_builder& root_signature_builder::with_parameter_table_sampler(
        std::uint32_t descriptor_count,
        std::uint32_t base_shader_register,
        std::uint32_t register_space,
        std::uint32_t offset_from_table_start,
        shader_visibility shader_vis)
    {
        std::uint32_t range_index = descriptor_ranges.size();
        descriptor_ranges.push_back(
            D3D12_DESCRIPTOR_RANGE{ .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE::D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
                                    .NumDescriptors = descriptor_count,
                                    .BaseShaderRegister = base_shader_register,
                                    .RegisterSpace = register_space,
                                    .OffsetInDescriptorsFromTableStart = offset_from_table_start });

        // Defer setting the pointer to the descriptor range to the build method because adding a new entry into
        // descriptor_ranges may change the in-memory pointer of the data in descriptor_ranges due to a resize of the
        // std::vector.
        root_parameters.push_back(D3D12_ROOT_PARAMETER{
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE::D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = { .NumDescriptorRanges = range_index, .pDescriptorRanges = nullptr },
            .ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(shader_vis) });

        return *this;
    }

    root_signature_builder& root_signature_builder::with_parameter_constant(
        std::uint32_t shader_register,
        std::uint32_t register_space,
        std::uint32_t constant_count_in_slot,
        shader_visibility shader_vis)
    {
        root_parameters.push_back(
            D3D12_ROOT_PARAMETER{ .ParameterType = D3D12_ROOT_PARAMETER_TYPE::D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                                  .Constants = { .ShaderRegister = shader_register,
                                                 .RegisterSpace = register_space,
                                                 .Num32BitValues = constant_count_in_slot, },
                                  .ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(shader_vis) });

        return *this;
    }

    root_signature_builder& root_signature_builder::with_parameter_constant_buffer_view(
        std::uint32_t shader_register, std::uint32_t register_space, shader_visibility shader_vis)
    {
        root_parameters.push_back(
            D3D12_ROOT_PARAMETER{ .ParameterType = D3D12_ROOT_PARAMETER_TYPE::D3D12_ROOT_PARAMETER_TYPE_CBV,
                                  .Descriptor = { .ShaderRegister = shader_register, .RegisterSpace = register_space },
                                  .ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(shader_vis) });

        return *this;
    }

    root_signature_builder& root_signature_builder::with_parameter_shader_resource_view(
        std::uint32_t shader_register, std::uint32_t register_space, shader_visibility shader_vis)
    {
        root_parameters.push_back(
            D3D12_ROOT_PARAMETER{ .ParameterType = D3D12_ROOT_PARAMETER_TYPE::D3D12_ROOT_PARAMETER_TYPE_SRV,
                                  .Descriptor = { .ShaderRegister = shader_register, .RegisterSpace = register_space },
                                  .ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(shader_vis) });

        return *this;
    }

    root_signature_builder& root_signature_builder::with_parameter_unordered_access_view(
        std::uint32_t shader_register, std::uint32_t register_space, shader_visibility shader_vis)
    {
        root_parameters.push_back(
            D3D12_ROOT_PARAMETER{ .ParameterType = D3D12_ROOT_PARAMETER_TYPE::D3D12_ROOT_PARAMETER_TYPE_UAV,
                                  .Descriptor = { .ShaderRegister = shader_register, .RegisterSpace = register_space },
                                  .ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(shader_vis) });

        return *this;
    }

    root_signature_builder& root_signature_builder::with_sampler(
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
        shader_visibility shader_vis)
    {
        sampler_descriptors.push_back(
            D3D12_STATIC_SAMPLER_DESC{ .Filter = static_cast<D3D12_FILTER>(filter_method),
                                       .AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(address_mode_u),
                                       .AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(address_mode_v),
                                       .AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(address_mode_w),
                                       .MipLODBias = mip_lod_bias,
                                       .MaxAnisotropy = max_anisotropy,
                                       .ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(compare_func),
                                       .BorderColor = static_cast<D3D12_STATIC_BORDER_COLOR>(border_color),
                                       .MinLOD = min_lod,
                                       .MaxLOD = max_lod,
                                       .ShaderRegister = shader_register,
                                       .RegisterSpace = register_space,
                                       .ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(shader_vis) });

        return *this;
    }

    root_signature_builder& root_signature_builder::with_flags(const flags<root_signature_flags> flags)
    {
        descriptor.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags.underlying_value());
        return *this;
    }

    root_signature root_signature_builder::build(graphics_device& device)
    {
        com_ptr<ID3D12RootSignature> new_root_signature{};

        // Resolve the D3D12_ROOT_PARAMETERs that refer to a descriptor table. In other words, fill the
        // D3D12_ROOT_PARAMETER with an actual pointer to a D3D12_DESCRIPTOR_RANGE.
        for (D3D12_ROOT_PARAMETER& parameter : root_parameters)
        {
            if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE::D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            {
                std::uint32_t range_index = parameter.DescriptorTable.NumDescriptorRanges;
                parameter.DescriptorTable.NumDescriptorRanges = 1;
                parameter.DescriptorTable.pDescriptorRanges = descriptor_ranges.data() + range_index;
            }
        }

        descriptor.pParameters = root_parameters.data();
        descriptor.NumParameters = root_parameters.size();
        descriptor.pStaticSamplers = sampler_descriptors.data();
        descriptor.NumStaticSamplers = sampler_descriptors.size();

        com_ptr<ID3DBlob> signature_blob{};
        com_ptr<ID3DBlob> error_blob{};
        throw_if_failed(
            D3D12SerializeRootSignature(&descriptor, D3D_ROOT_SIGNATURE_VERSION_1_1, &signature_blob, &error_blob));
        throw_if_failed(device.native_ptr()->CreateRootSignature(
            0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(), IID_PPV_ARGS(&new_root_signature)));

        return root_signature{ new_root_signature };
    }

    void root_signature_builder::reset()
    {
        root_parameters.clear();
        descriptor_ranges.clear();
        sampler_descriptors.clear();

        descriptor = D3D12_ROOT_SIGNATURE_DESC{};
    }
} // namespace graphics
