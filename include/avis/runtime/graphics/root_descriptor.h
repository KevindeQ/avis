#ifndef RUNTIME_GRAPHICS_ROOT_DESCRIPTOR_H
#    define RUNTIME_GRAHPICS_ROOT_DESCRIPTOR_H

#    include "avis/core/common.h"

namespace graphics
{
    class root_descriptor
    {
    public:
        root_descriptor(const D3D_ROOT_SIGNATURE_VERSION version, const D3D12_ROOT_SIGNATURE_FLAGS flags);

        ID3D12RootSignature* generate(ID3D12Device* const device);

    private:
        // TODO: For now only root signature version 1.0 are supported but this might be something for the future
        D3D_ROOT_SIGNATURE_VERSION version_;

        std::vector<D3D12_ROOT_PARAMETER> parameters_;
        std::vector<D3D12_STATIC_SAMPLER_DESC> samplers_;
        D3D12_ROOT_SIGNATURE_FLAGS flags_;
    };
} // namespace graphics

#endif
