#ifndef AVIS_RHI_D3D12_DEBUG_H
#define AVIS_RHI_D3D12_DEBUG_H

// Debug layer, info-queue routing and DRED (docs/design/09-rhi.md §20.3.3)

#include "avis/error.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"

#include <string>
#include <string_view>

namespace avis::rhi::d3d12
{
    // Lives inside device, at a stable address: the info-queue callback receives a pointer to it
    class debug_state
    {
    public:
        debug_state() = default;
        debug_state(const debug_state&) = delete;
        debug_state& operator=(const debug_state&) = delete;
        ~debug_state();

        // Before device creation: debug layer, GBV and DRED settings. Fails with debug_layer_unavailable when the
        // debug layer or GBV is requested and the Graphics Tools aren't installed.
        result<void> enable_before_device(const debug_configuration& configuration);

        // After device creation: ID3D12InfoQueue1::RegisterMessageCallback → the "rhi" logger, plus the deny list.
        // Does nothing without the debug layer.
        result<void> attach(device_interface& device);

        // Unregisters the callback; before the device goes
        void detach() noexcept;

        // Auto-breadcrumbs (per command list: last completed operation, the operations around it) and the
        // page-fault allocation list, as one multi-line report
        std::string device_removed_report(device_interface& device) const;

        bool debug_layer() const noexcept;
        bool object_names() const noexcept;
        bool break_on_error() const noexcept;

    private:
        com_ptr<ID3D12Debug1> debug_;
        com_ptr<ID3D12InfoQueue1> info_queue_;
        DWORD callback_cookie_ = 0;
        debug_configuration configuration_;
    };

    // SetName with UTF-16 conversion, skipped when object names are off or the name is empty
    void set_debug_name(const debug_state& debug, ID3D12Object& object, std::string_view name) noexcept;

    // UTF-16 → UTF-8 (adapter names, DRED debug names); empty on failure
    std::string to_utf8(std::wstring_view text);
} // namespace avis::rhi::d3d12

#endif
