#ifndef AVIS_RHI_D3D12_TIMESTAMP_QUERY_SET_STATE_H
#define AVIS_RHI_D3D12_TIMESTAMP_QUERY_SET_STATE_H

// The query heap and resolve buffer behind timestamp_query_set (docs/design/09-rhi.md §6, §24.4)

#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/resource_records.h"
#include "avis/rhi/d3d12/transient_upload.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <string>

namespace avis::rhi::d3d12
{
    struct timestamp_query_set_state;

    // A run of consecutive query indices one list wrote into one set: resolved together at submit. A list keeps several
    // runs per set when its indices aren't consecutive, so it never resolves an index it didn't write.
    struct timestamp_range
    {
        timestamp_query_set_state* set = nullptr;
        std::uint32_t first = 0;
        std::uint32_t end = 0; // one past the last index written
    };

    // Owned through a unique_ptr (§23.8)
    struct timestamp_query_set_state
    {
        transient_links links;
        com_ptr<ID3D12QueryHeap> heap; // TIMESTAMP, or COPY_QUEUE_TIMESTAMP for the copy queue
        buffer_record resolved; // count × 8 bytes, readback memory, mapped
        queue_type queue = queue_type::graphics;
        std::uint32_t count = 0;
        std::string name;
    };
} // namespace avis::rhi::d3d12

#endif
