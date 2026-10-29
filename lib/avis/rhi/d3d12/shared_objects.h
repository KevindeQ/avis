#ifndef AVIS_RHI_D3D12_SHARED_OBJECTS_H
#define AVIS_RHI_D3D12_SHARED_OBJECTS_H

// Cross-device sharing (docs/design/09-rhi.md §13, §21.5, §23.9): shared texture export and shared fences

#include "avis/error.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/completion_tracker.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/leaf_mutex.h"
#include "avis/rhi/d3d12/resource_records.h"
#include "avis/rhi/d3d12/unique_handle.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace avis::rhi::d3d12
{
    class device;
    class lifetime_manager;

    // Behind shared_fence. Owned through a unique_ptr and destroyed through the deferred release, because a queue may
    // still have a Signal or Wait on it in flight. Destroy it with no when_reached waiter left.
    struct shared_fence_state
    {
        com_ptr<ID3D12Fence> fence; // created with D3D12_FENCE_FLAG_SHARED, or opened from a handle
        timeline line; // waiters of when_reached (§23.4); attached to the tracker while non-empty
        unique_handle exported; // created on first export (GENERIC_ALL); closed with the fence
        leaf_mutex export_mutex{ "shared_fence_state::export_mutex" };
        device* owner = nullptr;
        lifetime_manager* lifetime = nullptr; // ~shared_fence hands the state to destroy
    };

    // CreateSharedHandle(resource, nullptr, GENERIC_ALL, nullptr, &handle) on the first call; later calls return the
    // same handle. The record owns it and closes it when released. Not thread-safe: the resource manager serialises.
    result<HANDLE> export_texture(device& device, texture_record& texture);

    result<std::unique_ptr<shared_fence_state>>
        create_fence(device& device, std::uint64_t initial_value, std::string_view debug_name);
    result<HANDLE> export_fence(device& device, shared_fence_state& fence); // once; thread-safe
    result<std::unique_ptr<shared_fence_state>> open_fence(device& device, HANDLE handle); // OpenSharedHandle
} // namespace avis::rhi::d3d12

#endif
