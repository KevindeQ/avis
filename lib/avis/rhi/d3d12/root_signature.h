#ifndef AVIS_RHI_D3D12_ROOT_SIGNATURE_H
#define AVIS_RHI_D3D12_ROOT_SIGNATURE_H

// Root signatures from descriptions, and the three indirect command signatures (docs/design/09-rhi.md §12.1, §22.3)

#include "avis/error.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/resource_records.h"
#include "avis/rhi/root_signature.h"

namespace avis::rhi::d3d12
{
    class device;

    // Validates the description (§12.1: 64 DWORDs, unique registers per class and visibility, border colors), then
    // serialises version 1.1 with CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED and no descriptor
    // tables. invalid_argument names the parameter; serialiser errors become native_error with its message.
    result<root_signature_record> create_root_signature(device& device, const root_signature_description& description);

    // Draw, draw indexed and dispatch. They change no root arguments (§7), so they're created without a root signature
    // and work with every one.
    class command_signatures
    {
    public:
        static result<command_signatures> create(device& device);

        ID3D12CommandSignature& draw() noexcept;
        ID3D12CommandSignature& draw_indexed() noexcept;
        ID3D12CommandSignature& dispatch() noexcept;

    private:
        com_ptr<ID3D12CommandSignature> draw_;
        com_ptr<ID3D12CommandSignature> draw_indexed_;
        com_ptr<ID3D12CommandSignature> dispatch_;
    };
} // namespace avis::rhi::d3d12

#endif
