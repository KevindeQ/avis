#include "avis/rhi/d3d12/root_signature.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/conversions/pipelines.h"
#include "avis/rhi/d3d12/conversions/samplers.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/error.h"

#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace avis::rhi::d3d12
{
    namespace
    {
        constexpr std::uint32_t maximum_dwords = 64;

        std::string label(const std::string_view name)
        {
            return name.empty() ? std::string{ "root signature" } : std::format("root signature '{}'", name);
        }

        avis::error invalid(std::string message)
        {
            return make_error(error_kind::invalid_argument, std::move(message));
        }

        // b (constants and constant buffers), t, u, s
        char register_class(const root_parameter_kind kind) noexcept
        {
            switch (kind)
            {
            case root_parameter_kind::constants:
            case root_parameter_kind::constant_buffer:
                return 'b';
            case root_parameter_kind::shader_resource:
                return 't';
            case root_parameter_kind::unordered_access:
                return 'u';
            }
            return 'b';
        }

        // `all` overlaps everything; vertex and pixel only overlap themselves (D-74)
        bool overlaps(const shader_visibility a, const shader_visibility b) noexcept
        {
            return a == shader_visibility::all || b == shader_visibility::all || a == b;
        }

        struct binding
        {
            char kind;
            std::uint32_t shader_register;
            std::uint32_t space;
            shader_visibility visibility;
            std::string what;
        };

        std::optional<D3D12_STATIC_BORDER_COLOR> static_border(const std::array<float, 4>& color) noexcept
        {
            if (color == std::array{ 0.0f, 0.0f, 0.0f, 0.0f })
            {
                return D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            }
            if (color == std::array{ 0.0f, 0.0f, 0.0f, 1.0f })
            {
                return D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
            }
            if (color == std::array{ 1.0f, 1.0f, 1.0f, 1.0f })
            {
                return D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
            }
            return std::nullopt;
        }

        result<void> validate(const root_signature_description& description)
        {
            const std::string name = label(description.debug_name);
            std::uint32_t dwords = 0;
            std::vector<binding> bindings;
            bindings.reserve(description.parameters.size() + description.static_samplers.size());

            for (std::size_t index = 0; index < description.parameters.size(); ++index)
            {
                const root_parameter& parameter = description.parameters[index];
                if (parameter.kind == root_parameter_kind::constants)
                {
                    if (parameter.constant_count == 0)
                    {
                        return std::unexpected(
                            invalid(std::format("{}: parameter {} has a constant_count of 0", name, index)));
                    }
                    dwords += parameter.constant_count;
                }
                else
                {
                    dwords += 2; // a root descriptor is a 64-bit address
                }
                bindings.push_back({ register_class(parameter.kind),
                                     parameter.shader_register,
                                     parameter.register_space,
                                     parameter.visibility,
                                     std::format("parameter {}", index) });
            }
            if (dwords > maximum_dwords)
            {
                return std::unexpected(invalid(std::format(
                    "{}: the parameters take {} DWORDs, more than {} (constants count one each, root descriptors two)",
                    name,
                    dwords,
                    maximum_dwords)));
            }

            for (std::size_t index = 0; index < description.static_samplers.size(); ++index)
            {
                const static_sampler& sampler = description.static_samplers[index];
                if (!static_border(sampler.sampler.border_color))
                {
                    return std::unexpected(invalid(std::format(
                        "{}: static sampler {} has a border color other than transparent black, opaque black or "
                        "opaque white",
                        name,
                        index)));
                }
                bindings.push_back({ 's',
                                     sampler.shader_register,
                                     sampler.register_space,
                                     sampler.visibility,
                                     std::format("static sampler {}", index) });
            }

            for (std::size_t first = 0; first < bindings.size(); ++first)
            {
                for (std::size_t second = first + 1; second < bindings.size(); ++second)
                {
                    const binding& a = bindings[first];
                    const binding& b = bindings[second];
                    if (a.kind == b.kind && a.shader_register == b.shader_register && a.space == b.space &&
                        overlaps(a.visibility, b.visibility))
                    {
                        return std::unexpected(invalid(std::format(
                            "{}: {} and {} both use {}{} in space {} with overlapping visibility",
                            name,
                            a.what,
                            b.what,
                            a.kind,
                            a.shader_register,
                            a.space)));
                    }
                }
            }
            return {};
        }

        D3D12_ROOT_PARAMETER1 to_native(const root_parameter& parameter) noexcept
        {
            D3D12_ROOT_PARAMETER1 native{};
            native.ShaderVisibility = d3d12::to_native(parameter.visibility);
            const D3D12_ROOT_DESCRIPTOR1 descriptor{ .ShaderRegister = parameter.shader_register,
                                                     .RegisterSpace = parameter.register_space,
                                                     .Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE };
            switch (parameter.kind)
            {
            case root_parameter_kind::constants:
                native.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                native.Constants = { .ShaderRegister = parameter.shader_register,
                                     .RegisterSpace = parameter.register_space,
                                     .Num32BitValues = parameter.constant_count };
                break;
            case root_parameter_kind::constant_buffer:
                native.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                native.Descriptor = descriptor;
                break;
            case root_parameter_kind::shader_resource:
                native.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                native.Descriptor = descriptor;
                break;
            case root_parameter_kind::unordered_access:
                native.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
                native.Descriptor = descriptor;
                break;
            }
            return native;
        }

        D3D12_STATIC_SAMPLER_DESC to_native(const static_sampler& sampler) noexcept
        {
            const sampler_description& state = sampler.sampler;
            return {
                .Filter = to_native_filter(state.minification, state.magnification, state.mip, state.compare.has_value()),
                .AddressU = d3d12::to_native(state.address_u),
                .AddressV = d3d12::to_native(state.address_v),
                .AddressW = d3d12::to_native(state.address_w),
                .MipLODBias = state.mip_bias,
                .MaxAnisotropy = state.maximum_anisotropy,
                .ComparisonFunc = state.compare ? d3d12::to_native(*state.compare) : D3D12_COMPARISON_FUNC_NEVER,
                .BorderColor = static_border(state.border_color).value_or(D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK),
                .MinLOD = 0.0f,
                .MaxLOD = D3D12_FLOAT32_MAX,
                .ShaderRegister = sampler.shader_register,
                .RegisterSpace = sampler.register_space,
                .ShaderVisibility = d3d12::to_native(sampler.visibility),
            };
        }

        result<com_ptr<ID3D12CommandSignature>> create_command_signature(
            device& device, const D3D12_INDIRECT_ARGUMENT_TYPE type, const UINT stride, const std::string_view name)
        {
            const D3D12_INDIRECT_ARGUMENT_DESC argument{ .Type = type };
            const D3D12_COMMAND_SIGNATURE_DESC description{
                .ByteStride = stride, .NumArgumentDescs = 1, .pArgumentDescs = &argument, .NodeMask = 0
            };
            com_ptr<ID3D12CommandSignature> signature;
            if (auto created = check(
                    device.native().CreateCommandSignature(&description, nullptr, IID_PPV_ARGS(&signature)),
                    std::format("CreateCommandSignature ({})", name));
                !created)
            {
                return std::unexpected(std::move(created.error()));
            }
            set_debug_name(device.debug(), *signature.get(), name);
            return signature;
        }
    } // namespace

    result<root_signature_record> create_root_signature(device& device, const root_signature_description& description)
    {
        if (auto valid = validate(description); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }

        std::vector<D3D12_ROOT_PARAMETER1> parameters;
        parameters.reserve(description.parameters.size());
        for (const root_parameter& parameter : description.parameters)
        {
            parameters.push_back(to_native(parameter));
        }
        std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
        samplers.reserve(description.static_samplers.size());
        for (const static_sampler& sampler : description.static_samplers)
        {
            samplers.push_back(to_native(sampler));
        }

        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC native{
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = { .NumParameters = static_cast<UINT>(parameters.size()),
                          .pParameters = parameters.data(),
                          .NumStaticSamplers = static_cast<UINT>(samplers.size()),
                          .pStaticSamplers = samplers.data(),
                          .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                   D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED },
        };

        const std::string name = label(description.debug_name);
        com_ptr<ID3DBlob> blob;
        com_ptr<ID3DBlob> errors;
        if (const HRESULT serialised = D3D12SerializeVersionedRootSignature(&native, &blob, &errors); FAILED(serialised))
        {
            std::string message =
                errors ? std::string{ static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize() } : "";
            while (!message.empty() && (message.back() == '\0' || message.back() == '\n'))
            {
                message.pop_back();
            }
            return std::unexpected(make_error(
                error_kind::native_error,
                std::format("{}: D3D12SerializeVersionedRootSignature failed (HRESULT 0x{:08X}): {}",
                            name,
                            static_cast<std::uint32_t>(serialised),
                            message)));
        }

        root_signature_record record;
        if (auto created = check(
                device.native().CreateRootSignature(
                    0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&record.native)),
                std::format("CreateRootSignature ({})", name));
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        record.parameters.assign(description.parameters.begin(), description.parameters.end());
        record.name = std::string{ description.debug_name };
        set_debug_name(device.debug(), *record.native.get(), record.name);
        return record;
    }

    result<command_signatures> command_signatures::create(device& device)
    {
        command_signatures signatures;
        auto draw = create_command_signature(
            device, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, sizeof(D3D12_DRAW_ARGUMENTS), "draw command signature");
        if (!draw)
        {
            return std::unexpected(std::move(draw.error()));
        }
        auto draw_indexed = create_command_signature(
            device,
            D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
            sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
            "draw indexed command signature");
        if (!draw_indexed)
        {
            return std::unexpected(std::move(draw_indexed.error()));
        }
        auto dispatch = create_command_signature(
            device, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, sizeof(D3D12_DISPATCH_ARGUMENTS), "dispatch command signature");
        if (!dispatch)
        {
            return std::unexpected(std::move(dispatch.error()));
        }
        signatures.draw_ = std::move(*draw);
        signatures.draw_indexed_ = std::move(*draw_indexed);
        signatures.dispatch_ = std::move(*dispatch);
        return signatures;
    }

    ID3D12CommandSignature& command_signatures::draw() noexcept
    {
        return *draw_.get();
    }

    ID3D12CommandSignature& command_signatures::draw_indexed() noexcept
    {
        return *draw_indexed_.get();
    }

    ID3D12CommandSignature& command_signatures::dispatch() noexcept
    {
        return *dispatch_.get();
    }
} // namespace avis::rhi::d3d12
