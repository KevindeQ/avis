#include "avis/rhi/d3d12/shared_objects.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/device.h"

#include <mutex>
#include <utility>

namespace avis::rhi::d3d12
{
    namespace
    {
        result<std::unique_ptr<shared_fence_state>> make_state(device& device, com_ptr<ID3D12Fence> fence)
        {
            auto state = std::make_unique<shared_fence_state>();
            state->fence = std::move(fence);
            state->line.fence = state->fence.get();
            state->line.index = timeline::shared_index;
            state->owner = &device;
            return state;
        }
    } // namespace

    result<HANDLE> export_texture(device& device, texture_record& texture)
    {
        if (!texture.shared_handle)
        {
            HANDLE handle = nullptr;
            if (auto created = check_device(
                    device,
                    device.native().CreateSharedHandle(texture.resource.get(), nullptr, GENERIC_ALL, nullptr, &handle),
                    "CreateSharedHandle (texture)");
                !created)
            {
                return std::unexpected(std::move(created.error()));
            }
            texture.shared_handle.reset(handle);
        }
        return texture.shared_handle.get();
    }

    result<std::unique_ptr<shared_fence_state>>
        create_fence(device& device, const std::uint64_t initial_value, const std::string_view debug_name)
    {
        com_ptr<ID3D12Fence> fence;
        if (auto created = check_device(
                device,
                device.native().CreateFence(initial_value, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)),
                "CreateFence (shared)");
            !created)
        {
            return std::unexpected(std::move(created.error()));
        }
        set_debug_name(device.debug(), *fence.get(), debug_name);
        return make_state(device, std::move(fence));
    }

    result<HANDLE> export_fence(device& device, shared_fence_state& fence)
    {
        const std::lock_guard lock{ fence.export_mutex };
        if (!fence.exported)
        {
            HANDLE handle = nullptr;
            if (auto created = check_device(
                    device,
                    device.native().CreateSharedHandle(fence.fence.get(), nullptr, GENERIC_ALL, nullptr, &handle),
                    "CreateSharedHandle (fence)");
                !created)
            {
                return std::unexpected(std::move(created.error()));
            }
            fence.exported.reset(handle);
        }
        return fence.exported.get();
    }

    result<std::unique_ptr<shared_fence_state>> open_fence(device& device, const HANDLE handle)
    {
        com_ptr<ID3D12Fence> fence;
        if (auto opened = check_device(
                device, device.native().OpenSharedHandle(handle, IID_PPV_ARGS(&fence)), "OpenSharedHandle (fence)");
            !opened)
        {
            return std::unexpected(std::move(opened.error()));
        }
        return make_state(device, std::move(fence));
    }
} // namespace avis::rhi::d3d12
