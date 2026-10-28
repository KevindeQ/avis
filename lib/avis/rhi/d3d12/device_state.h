#ifndef AVIS_RHI_D3D12_DEVICE_STATE_H
#define AVIS_RHI_D3D12_DEVICE_STATE_H

// Composition root of the private RHI: owns the subsystems, wires them, and orders their destruction
// (docs/design/09-rhi.md §20.3.1, §19.3). Each later step of task 6 appends its subsystem in construction order.

#include "avis/error.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/d3d12/device.h"

#include <cstddef>
#include <memory>

namespace avis::rhi::d3d12
{
    class device_state
    {
    public:
        static result<std::unique_ptr<device_state>>
            create(const render_hardware_interface_configuration& configuration);

        // wait_idle, then the members in reverse construction order
        ~device_state();

        device_state(const device_state&) = delete;
        device_state& operator=(const device_state&) = delete;

        // For the public views and RHI tests (through render_hardware_interface::state)
        d3d12::device& device() noexcept;

        // completion_driver::poll_completions(). Nothing to complete until the queues (6.4).
        std::size_t poll_completions() noexcept;

        // completion_driver::has_pending_work(). Nothing pending until the queues (6.4) and releases (6.5).
        bool has_pending_work() const noexcept;

        // render_hardware_interface::wait_idle() and the destructor. Nothing to wait for until the queues (6.4).
        void wait_idle();

    private:
        device_state() = default;

        // Construction order; destroyed in reverse
        std::unique_ptr<d3d12::device> device_;
    };
} // namespace avis::rhi::d3d12

#endif
