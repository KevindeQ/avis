#include "avis/runtime/graphics/pipeline_state.h"

namespace graphics
{
    pipeline_state::pipeline_state(com_ptr<ID3D12PipelineState> state) : native_pipeline_state{ state } {}

    graphics_pipeline_state_builder::graphics_pipeline_state_builder() : descriptor{}
    {
        // Only support single GPU operation
        descriptor.NodeMask = 0;

        // TODO: Add support for cached pipeline states
        descriptor.CachedPSO.pCachedBlob = nullptr;
        descriptor.CachedPSO.CachedBlobSizeInBytes = 0;
    }

    /*pipeline_state pipeline_state_builder::create_pipeline_state(const graphics_pipeline_state_descriptor& desc)
    {
        return pipeline_state{};
    }

    pipeline_state pipeline_state_builder::create_pipeline_state(const compute_pipeline_state_descriptor& desc)
    {
        return pipeline_state{};
    }*/

    pipeline_state graphics_pipeline_state_builder::build(graphics_device& device)
    {
        com_ptr<ID3D12PipelineState> pso{};
        throw_if_failed(device.native_ptr()->CreateGraphicsPipelineState(&descriptor, IID_PPV_ARGS(&pso)));

        return pipeline_state{ pso };
    }

    compute_pipeline_state_builder::compute_pipeline_state_builder() : descriptor{}
    {
        // Only support single GPU operation
        descriptor.NodeMask = 0;

        // TODO: Add support for cached pipeline states
        descriptor.CachedPSO.pCachedBlob = nullptr;
        descriptor.CachedPSO.CachedBlobSizeInBytes = 0;
    }

    compute_pipeline_state_builder& compute_pipeline_state_builder::with_root_signature(const root_signature& signature)
    {
        descriptor.pRootSignature = signature.native_ptr().get();

        return *this;
    }

    compute_pipeline_state_builder&
        compute_pipeline_state_builder::with_compute_shader(const shader_ref& compute_shader)
    {
        std::span<const unsigned char> byte_code = compute_shader.byte_code();
        descriptor.CS.pShaderBytecode = byte_code.data();
        descriptor.CS.BytecodeLength = byte_code.size();

        return *this;
    }

    pipeline_state compute_pipeline_state_builder::build(graphics_device& device)
    {
        com_ptr<ID3D12PipelineState> pso{};
        throw_if_failed(device.native_ptr()->CreateComputePipelineState(&descriptor, IID_PPV_ARGS(&pso)));

        return pipeline_state{ pso };
    }
} // namespace graphics
