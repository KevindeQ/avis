#ifndef AVIS_RHI_D3D12_CONVERSIONS_QUEUES_H
#define AVIS_RHI_D3D12_CONVERSIONS_QUEUES_H

// queue_type → D3D12_COMMAND_LIST_TYPE (docs/design/09-rhi.md §20.2.3)

#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/types.h"

namespace avis::rhi::d3d12
{
    D3D12_COMMAND_LIST_TYPE to_native(queue_type value) noexcept;
} // namespace avis::rhi::d3d12

#endif
