#ifndef AVIS_RHI_D3D12_NATIVE_ACCESS_H
#define AVIS_RHI_D3D12_NATIVE_ACCESS_H

// Native objects behind the public API, for RHI internals and RHI tests only; never for applications
// (docs/design/09-rhi.md §20.2.4). Code that needs the device calls rhi.state().device().native().

#include "avis/rhi/command_list.h"
#include "avis/rhi/d3d12/command_list_pool.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/types.h"

namespace avis::rhi::d3d12
{
    inline ID3D12CommandQueue&
        native_queue(render_hardware_interface& hardware_interface, const queue_type queue) noexcept
    {
        return hardware_interface.state().queues()[queue].native();
    }

    // nullptr for a stale handle
    inline ID3D12Resource*
        native_resource(render_hardware_interface& hardware_interface, const texture_handle texture) noexcept
    {
        const texture_record* const record = hardware_interface.state().resources().find(texture);
        return record != nullptr ? record->resource.get() : nullptr;
    }

    inline ID3D12Resource*
        native_resource(render_hardware_interface& hardware_interface, const buffer_handle buffer) noexcept
    {
        const buffer_record* const record = hardware_interface.state().resources().find(buffer);
        return record != nullptr ? record->resource.get() : nullptr;
    }

    struct recorder_access
    {
        template<queue_type Queue>
        static command_list_record* record(const command_recorder<Queue>& recorder) noexcept
        {
            return recorder.record_;
        }
    };

    // nullptr for a submitted, discarded or moved-from list
    inline graphics_list_interface* native_list(const command_list<queue_type::graphics>& list) noexcept
    {
        command_list_record* const record = recorder_access::record(list);
        return record != nullptr && record->state.load(std::memory_order_acquire) == list_state::recording ?
                   record->list.get() :
                   nullptr;
    }
} // namespace avis::rhi::d3d12

#endif
