#ifndef AVIS_RHI_D3D12_COMPLETION_SOURCE_H
#define AVIS_RHI_D3D12_COMPLETION_SOURCE_H

// What classes that only check completions depend on (docs/design/09-rhi.md §20.3.1): the command list pool and the
// transient objects' reset checks. Implemented by device_queues; a small interface so unit tests can fake it.

#include "avis/rhi/queue.h"

#include <array>
#include <cstdint>

namespace avis::rhi::d3d12
{
    class completion_source
    {
    public:
        // value 0 is always complete
        virtual bool is_complete(synchronization_point point) const noexcept = 0;

    protected:
        completion_source() noexcept = default;
        ~completion_source() = default;
        completion_source(const completion_source&) = default;
        completion_source& operator=(const completion_source&) = default;
    };

    // One timeline value per queue, indexed by queue_type: the values that make a deferred release safe to free
    // (§23.8), and each queue's last submitted or completed value
    using release_tag = std::array<std::uint64_t, 3>;
} // namespace avis::rhi::d3d12

#endif
