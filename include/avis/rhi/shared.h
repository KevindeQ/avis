#ifndef AVIS_RHI_SHARED_H
#define AVIS_RHI_SHARED_H

// Cross-device sharing: shared textures and fences (docs/design/09-rhi.md §13). The Visualizer's interop protocol with
// Qt is in 12 §3. Free of D3D12 and <windows.h>.

#include "avis/error.h"
#include "avis/rhi/detail/gpu_wait_sender.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/types.h"

#include <cstdint>

namespace avis::rhi
{
    class render_hardware_interface;
    template<queue_type>
    class queue;

    namespace d3d12
    {
        class device_state;
        struct shared_fence_state;
    } // namespace d3d12

    // An NT handle value (HANDLE), opaque to keep <windows.h> out. Non-owning: the exporting texture or fence owns it
    // and closes it when destroyed.
    struct shared_handle
    {
        void* value = nullptr;
    };

    // A fence that another device can open and signal. Move-only; destroyed through the RHI's deferred release, because
    // a queue may still have a Signal or Wait on it in flight. Destroy it with no when_reached sender still pending.
    class shared_fence
    {
    public:
        shared_fence(shared_fence&& other) noexcept;
        shared_fence& operator=(shared_fence&& other) noexcept;
        shared_fence(const shared_fence&) = delete;
        shared_fence& operator=(const shared_fence&) = delete;
        ~shared_fence();

        // ID3D12Fence::GetCompletedValue: any device's signals
        std::uint64_t completed_value() const noexcept;

    private:
        friend class sharing;
        template<queue_type>
        friend class queue;

        explicit shared_fence(d3d12::shared_fence_state* state) noexcept;
        void release() noexcept;

        d3d12::shared_fence_state* state_ = nullptr;
    };

    // A view (§4.3): non-owning, copyable, built from the RHI and never outliving it
    class sharing
    {
    public:
        explicit sharing(render_hardware_interface& hardware_interface) noexcept;

        // Requires texture_usage::shared (invalid_argument otherwise); committed with D3D12_HEAP_FLAG_SHARED
        result<texture_handle> create_shared_texture(const texture_description& description);
        // Created on the first call (GENERIC_ALL), the same value afterwards
        result<shared_handle> export_shared_handle(texture_handle texture);

        result<shared_fence> create_shared_fence(std::uint64_t initial_value = 0);
        // OpenSharedHandle on a fence another device exported; the RHI doesn't take ownership of the handle
        result<shared_fence> import_shared_fence(shared_handle handle);
        result<shared_handle> export_shared_handle(const shared_fence& fence);

        // Like when_complete (§5.1), for a value of a shared fence, including values another device signals
        gpu_wait_sender when_reached(const shared_fence& fence, std::uint64_t value);

    private:
        d3d12::device_state* state_;
    };
} // namespace avis::rhi

#endif
