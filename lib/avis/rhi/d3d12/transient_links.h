#ifndef AVIS_RHI_D3D12_TRANSIENT_LINKS_H
#define AVIS_RHI_D3D12_TRANSIENT_LINKS_H

// The links every transient object's state carries (docs/design/09-rhi.md §23.6). A header of its own because the
// descriptor range state (descriptor_heaps.h) needs it, and transient_upload.h needs the buffer record, which needs the
// descriptor heaps.

namespace avis::rhi::d3d12
{
    class completion_source;
    class lifetime_manager;

    // Filled in by the transient_objects view at creation, so the public object holds only its state pointer and never
    // the device_state (§6)
    struct transient_links
    {
        const completion_source* completions = nullptr; // reset check
        lifetime_manager* lifetime = nullptr; // the public destructor hands the state to destroy (§23.8)
    };
} // namespace avis::rhi::d3d12

#endif
