#ifndef AVIS_RHI_D3D12_TRANSIENT_UPLOAD_H
#define AVIS_RHI_D3D12_TRANSIENT_UPLOAD_H

// Transient upload memory and reset checks (docs/design/09-rhi.md §23.6). Only transient_links so far: the descriptor
// range state (6.6) carries it. The linear upload allocator's state and check_reset_allowed come with 6.9.

namespace avis::rhi::d3d12
{
    class completion_source;
    class lifetime_manager;

    // What every transient object's state needs beyond its own data. Filled in by the transient_objects view at
    // creation, so the public object holds only its state pointer and never the device_state (§6).
    struct transient_links
    {
        const completion_source* completions = nullptr; // reset check
        lifetime_manager* lifetime = nullptr; // the public destructor hands the state to destroy (§23.8)
    };
} // namespace avis::rhi::d3d12

#endif
