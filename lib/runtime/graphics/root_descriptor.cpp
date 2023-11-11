#include "avis/runtime/graphics/root_descriptor.h"

namespace graphics
{
    root_descriptor::root_descriptor(const D3D_ROOT_SIGNATURE_VERSION version, const D3D12_ROOT_SIGNATURE_FLAGS flags) :
        version_{version},
        flags_{flags}
    {}

    ID3D12RootSignature* root_descriptor::generate(ID3D12Device* const device)
    {
        // TODO: For now only root signature version 1.0 are supported but this might be something for the future
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_signature_desc = {};
        root_signature_desc.Version = D3D_ROOT_SIGNATURE_VERSION::D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_signature_desc.Desc_1_0.pParameters = parameters_.data();
        root_signature_desc.Desc_1_0.NumParameters = parameters_.size();
        root_signature_desc.Desc_1_0.pStaticSamplers = samplers_.data();
        root_signature_desc.Desc_1_0.NumStaticSamplers = samplers_.size();
        root_signature_desc.Desc_1_0.Flags = flags_;

        ID3DBlob* serialized_root_signature = nullptr;
        ID3DBlob* errors = nullptr;
        D3D12SerializeVersionedRootSignature(&root_signature_desc, &serialized_root_signature, &errors);

        ID3D12RootSignature* root_signature = nullptr;
        device->CreateRootSignature(
            0,
            serialized_root_signature->GetBufferPointer(),
            serialized_root_signature->GetBufferSize(),
            IID_PPV_ARGS(&root_signature));

        return root_signature;
    }
} // namespace graphics
