#ifndef AVIS_RHI_D3D12_TRANSIENT_UPLOAD_H
#define AVIS_RHI_D3D12_TRANSIENT_UPLOAD_H

// Transient upload memory and reset checks (docs/design/09-rhi.md §23.6)

#include "avis/rhi/d3d12/resource_records.h"
#include "avis/rhi/d3d12/transient_links.h"
#include "avis/rhi/queue.h"

#include <atomic>
#include <cstddef>
#include <span>
#include <string_view>

namespace avis::rhi::d3d12
{
    class completion_source;

    // Behind linear_upload_allocator: an atomic bump pointer over one persistently mapped UPLOAD buffer. Holds atomics,
    // so it's never moved: owned through a unique_ptr, also in the deferred release (§23.8).
    struct linear_upload_allocator_state
    {
        transient_links links;
        buffer_record buffer; // released through the deferred release with the public object
        std::atomic<std::size_t> offset{ 0 };
        std::atomic<std::size_t> high_water_mark{ 0 };
        std::atomic<bool> exhaustion_logged{ false }; // once per reset cycle
    };

    // The check every transient reset makes: debug builds assert each point is complete (is_complete refreshes the
    // cached fence value once); release builds do nothing
    void check_reset_allowed(
        const completion_source& completions,
        std::span<const synchronization_point> uses,
        std::string_view object_name) noexcept;
} // namespace avis::rhi::d3d12

#endif
