#include "avis/rhi/d3d12/pipeline_factory.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/conversions/formats.h"
#include "avis/rhi/d3d12/conversions/pipelines.h"
#include "avis/rhi/d3d12/conversions/samplers.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/error.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        std::string label(const std::string_view kind, const std::string_view name)
        {
            return name.empty() ? std::string{ kind } : std::format("{} '{}'", kind, name);
        }

        avis::error invalid(std::string message)
        {
            return make_error(error_kind::invalid_argument, std::move(message));
        }

        // One subobject of a pipeline state stream, laid out like CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT
        template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class Value>
        struct alignas(void*) subobject
        {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
            Value value{};
        };

        struct graphics_stream
        {
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root_signature;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vertex_shader;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> pixel_shader;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> depth_stencil;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> render_targets;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
        };

        struct compute_stream
        {
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root_signature;
            subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, D3D12_SHADER_BYTECODE> compute_shader;
        };

        D3D12_SHADER_BYTECODE bytecode(const std::span<const std::byte> code) noexcept
        {
            return { .pShaderBytecode = code.data(), .BytecodeLength = code.size() };
        }

        D3D12_CULL_MODE to_native(const cull_mode value) noexcept
        {
            switch (value)
            {
            case cull_mode::none:
                return D3D12_CULL_MODE_NONE;
            case cull_mode::front:
                return D3D12_CULL_MODE_FRONT;
            case cull_mode::back:
                return D3D12_CULL_MODE_BACK;
            }
            return D3D12_CULL_MODE_BACK;
        }

        D3D12_BLEND to_native(const blend_factor value) noexcept
        {
            switch (value)
            {
            case blend_factor::zero:
                return D3D12_BLEND_ZERO;
            case blend_factor::one:
                return D3D12_BLEND_ONE;
            case blend_factor::source_alpha:
                return D3D12_BLEND_SRC_ALPHA;
            case blend_factor::inverse_source_alpha:
                return D3D12_BLEND_INV_SRC_ALPHA;
            case blend_factor::destination_alpha:
                return D3D12_BLEND_DEST_ALPHA;
            case blend_factor::inverse_destination_alpha:
                return D3D12_BLEND_INV_DEST_ALPHA;
            case blend_factor::source_color:
                return D3D12_BLEND_SRC_COLOR;
            case blend_factor::inverse_source_color:
                return D3D12_BLEND_INV_SRC_COLOR;
            }
            return D3D12_BLEND_ONE;
        }

        D3D12_BLEND_OP to_native(const blend_operation value) noexcept
        {
            switch (value)
            {
            case blend_operation::add:
                return D3D12_BLEND_OP_ADD;
            case blend_operation::subtract:
                return D3D12_BLEND_OP_SUBTRACT;
            case blend_operation::reverse_subtract:
                return D3D12_BLEND_OP_REV_SUBTRACT;
            case blend_operation::minimum:
                return D3D12_BLEND_OP_MIN;
            case blend_operation::maximum:
                return D3D12_BLEND_OP_MAX;
            }
            return D3D12_BLEND_OP_ADD;
        }

        avis::error creation_failed(const std::string& name, const HRESULT result)
        {
            return make_error(
                error_kind::pipeline_creation_failed,
                std::format("{}: {}", name, d3d12::make_error(result, "CreatePipelineState").message));
        }

        result<com_ptr<ID3D12PipelineState>>
            create_from_stream(device& device, void* const stream, const std::size_t size, const std::string& name)
        {
            const D3D12_PIPELINE_STATE_STREAM_DESC description{ .SizeInBytes = size, .pPipelineStateSubobjectStream = stream };
            com_ptr<ID3D12PipelineState> state;
            if (const HRESULT created = device.native().CreatePipelineState(&description, IID_PPV_ARGS(&state));
                FAILED(created))
            {
                if (is_device_removed_code(created))
                {
                    device.report_removed("CreatePipelineState");
                }
                return std::unexpected(creation_failed(name, created));
            }
            return state;
        }
    } // namespace

    result<com_ptr<ID3D12PipelineState>> create_pipeline_state(
        device& device, const root_signature_record& root, const graphics_pipeline_description& description)
    {
        const graphics_pipeline_state& state = description.state;
        graphics_stream stream;
        stream.root_signature.value = root.native.get();
        stream.vertex_shader.value = bytecode(description.vertex_shader);
        stream.pixel_shader.value = bytecode(description.pixel_shader);
        stream.rasterizer.value = {
            .FillMode = state.rasterizer.fill == fill_mode::wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID,
            .CullMode = to_native(state.rasterizer.cull),
            .FrontCounterClockwise = state.rasterizer.front_counter_clockwise ? TRUE : FALSE,
            .DepthBias = state.rasterizer.depth_bias,
            .DepthBiasClamp = state.rasterizer.depth_bias_clamp,
            .SlopeScaledDepthBias = state.rasterizer.slope_scaled_depth_bias,
            .DepthClipEnable = state.rasterizer.depth_clip ? TRUE : FALSE,
            .MultisampleEnable = FALSE,
            .AntialiasedLineEnable = FALSE,
            .ForcedSampleCount = 0,
            .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
        };
        const D3D12_DEPTH_STENCILOP_DESC keep{ .StencilFailOp = D3D12_STENCIL_OP_KEEP,
                                               .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
                                               .StencilPassOp = D3D12_STENCIL_OP_KEEP,
                                               .StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS };
        stream.depth_stencil.value = {
            .DepthEnable = state.depth.test ? TRUE : FALSE,
            .DepthWriteMask = state.depth.test && state.depth.write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO,
            .DepthFunc = d3d12::to_native(state.depth.compare),
            .StencilEnable = FALSE,
            .StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK,
            .StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK,
            .FrontFace = keep,
            .BackFace = keep,
            .DepthBoundsTestEnable = FALSE,
        };
        stream.blend.value.AlphaToCoverageEnable = FALSE;
        stream.blend.value.IndependentBlendEnable = TRUE;
        for (std::size_t target = 0; target < state.blend.size(); ++target)
        {
            const blend_state& blend = state.blend[target];
            stream.blend.value.RenderTarget[target] = {
                .BlendEnable = blend.enable ? TRUE : FALSE,
                .LogicOpEnable = FALSE,
                .SrcBlend = to_native(blend.source_color),
                .DestBlend = to_native(blend.destination_color),
                .BlendOp = to_native(blend.color_operation),
                .SrcBlendAlpha = to_native(blend.source_alpha),
                .DestBlendAlpha = to_native(blend.destination_alpha),
                .BlendOpAlpha = to_native(blend.alpha_operation),
                .LogicOp = D3D12_LOGIC_OP_NOOP,
                .RenderTargetWriteMask = blend.write_mask,
            };
        }
        stream.render_targets.value.NumRenderTargets = state.render_target_count;
        for (std::uint32_t target = 0; target < state.render_target_count; ++target)
        {
            stream.render_targets.value.RTFormats[target] = d3d12::to_native(state.render_target_formats[target]);
        }
        stream.depth_format.value =
            state.depth_format == format::unknown ? DXGI_FORMAT_UNKNOWN : d3d12::to_native(state.depth_format);
        stream.topology.value = to_native_topology_type(state.topology);
        stream.sample.value = { .Count = 1, .Quality = 0 };
        stream.sample_mask.value = UINT_MAX;

        return create_from_stream(device, &stream, sizeof(stream), label("graphics pipeline", description.debug_name));
    }

    result<com_ptr<ID3D12PipelineState>> create_pipeline_state(
        device& device, const root_signature_record& root, const compute_pipeline_description& description)
    {
        compute_stream stream;
        stream.root_signature.value = root.native.get();
        stream.compute_shader.value = bytecode(description.compute_shader);
        return create_from_stream(device, &stream, sizeof(stream), label("compute pipeline", description.debug_name));
    }

    pipeline_factory::pipeline_factory(device& device, d3d12::command_signatures signatures) noexcept :
        device_{ device },
        command_signatures_{ std::move(signatures) }
    {}

    result<std::unique_ptr<pipeline_factory>> pipeline_factory::create(device& device)
    {
        auto signatures = command_signatures::create(device);
        if (!signatures)
        {
            return std::unexpected(std::move(signatures.error()));
        }
        return std::unique_ptr<pipeline_factory>{ new pipeline_factory{ device, std::move(*signatures) } };
    }

    result<root_signature_handle> pipeline_factory::create_root_signature(const root_signature_description& description)
    {
        auto record = d3d12::create_root_signature(device_, description);
        if (!record)
        {
            return std::unexpected(std::move(record.error()));
        }
        auto bits = root_signatures_.insert(std::move(*record));
        if (!bits)
        {
            return std::unexpected(std::move(bits.error()));
        }
        return to_handle<root_signature_handle>(*bits);
    }

    result<graphics_pipeline_handle> pipeline_factory::create_pipeline(const graphics_pipeline_description& description)
    {
        const std::string name = label("graphics pipeline", description.debug_name);
        const root_signature_record* const root = root_signatures_.find(to_bits(description.root_signature));
        if (root == nullptr)
        {
            return std::unexpected(
                make_error(error_kind::invalid_handle, std::format("{}: stale or invalid root signature handle", name)));
        }
        if (description.vertex_shader.empty())
        {
            return std::unexpected(invalid(std::format("{}: the vertex shader is empty", name)));
        }
        const graphics_pipeline_state& state = description.state;
        if (state.render_target_count > 8)
        {
            return std::unexpected(
                invalid(std::format("{}: {} render targets, at most 8", name, state.render_target_count)));
        }
        for (std::uint32_t target = 0; target < state.render_target_count; ++target)
        {
            if (state.render_target_formats[target] == format::unknown)
            {
                return std::unexpected(invalid(std::format("{}: render target {} has no format", name, target)));
            }
        }
        if (state.depth.test && state.depth_format == format::unknown)
        {
            return std::unexpected(invalid(std::format("{}: depth testing is on but depth_format is unknown", name)));
        }

        auto pipeline_state = create_pipeline_state(device_, *root, description);
        if (!pipeline_state)
        {
            return std::unexpected(std::move(pipeline_state.error()));
        }
        set_debug_name(device_.debug(), *pipeline_state->get(), description.debug_name);
        auto bits = graphics_pipelines_.insert(graphics_pipeline_record{ .state = std::move(*pipeline_state),
                                                                         .root_signature = root,
                                                                         .name = std::string{ description.debug_name },
                                                                         .topology = d3d12::to_native(state.topology) });
        if (!bits)
        {
            return std::unexpected(std::move(bits.error()));
        }
        return to_handle<graphics_pipeline_handle>(*bits);
    }

    result<compute_pipeline_handle> pipeline_factory::create_pipeline(const compute_pipeline_description& description)
    {
        const std::string name = label("compute pipeline", description.debug_name);
        const root_signature_record* const root = root_signatures_.find(to_bits(description.root_signature));
        if (root == nullptr)
        {
            return std::unexpected(
                make_error(error_kind::invalid_handle, std::format("{}: stale or invalid root signature handle", name)));
        }
        if (description.compute_shader.empty())
        {
            return std::unexpected(invalid(std::format("{}: the compute shader is empty", name)));
        }

        auto pipeline_state = create_pipeline_state(device_, *root, description);
        if (!pipeline_state)
        {
            return std::unexpected(std::move(pipeline_state.error()));
        }
        set_debug_name(device_.debug(), *pipeline_state->get(), description.debug_name);
        auto bits = compute_pipelines_.insert(compute_pipeline_record{
            .state = std::move(*pipeline_state), .root_signature = root, .name = std::string{ description.debug_name } });
        if (!bits)
        {
            return std::unexpected(std::move(bits.error()));
        }
        return to_handle<compute_pipeline_handle>(*bits);
    }

    const root_signature_record* pipeline_factory::find(const root_signature_handle signature) const noexcept
    {
        return root_signatures_.find(to_bits(signature));
    }

    const graphics_pipeline_record* pipeline_factory::find(const graphics_pipeline_handle pipeline) const noexcept
    {
        return graphics_pipelines_.find(to_bits(pipeline));
    }

    const compute_pipeline_record* pipeline_factory::find(const compute_pipeline_handle pipeline) const noexcept
    {
        return compute_pipelines_.find(to_bits(pipeline));
    }

    d3d12::command_signatures& pipeline_factory::command_signatures() noexcept
    {
        return command_signatures_;
    }
} // namespace avis::rhi::d3d12
