#ifndef AVIS_RHI_D3D12_DEVICE_STATE_H
#define AVIS_RHI_D3D12_DEVICE_STATE_H

// Composition root of the private RHI: owns the subsystems, wires them, and orders their destruction
// (docs/design/09-rhi.md §20.3.1, §19.3). Each later step of task 6 appends its subsystem in construction order.

#include "avis/error.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/d3d12/completion_tracker.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/d3d12/device_queues.h"
#include "avis/rhi/d3d12/lifetime_manager.h"

#include <cstddef>
#include <memory>

namespace avis::rhi::d3d12
{
    class device_state
    {
    public:
        static result<std::unique_ptr<device_state>>
            create(const render_hardware_interface_configuration& configuration);

        // wait_idle, lifetime_manager::drain, then the members in reverse construction order (§19.3)
        ~device_state();

        device_state(const device_state&) = delete;
        device_state& operator=(const device_state&) = delete;

        // For the public views and RHI tests (through render_hardware_interface::state)
        d3d12::device& device() noexcept;
        device_queues& queues() noexcept;
        completion_tracker& completions() noexcept;
        lifetime_manager& lifetime() noexcept;

        // completion_driver::poll_completions() (§5.2): the tracker, then lifetime housekeeping. When housekeeping
        // left ready releases (D-77), completion_tracker::signal, so the driver calls again.
        std::size_t poll_completions() noexcept;

        // completion_driver::has_pending_work(): waiters in the tracker, or deferred releases not yet freed
        bool has_pending_work() const noexcept;

        // render_hardware_interface::wait_idle() and the destructor: waits on every queue fence with a private event,
        // then poll_completions(). Throws device_lost after device removal.
        void wait_idle();

    private:
        device_state() = default;

        // Construction order; destroyed in reverse. descriptors_ (6.6) goes between completions_ and lifetime_.
        std::unique_ptr<d3d12::device> device_;
        std::unique_ptr<device_queues> queues_;
        std::unique_ptr<completion_tracker> completions_;
        std::unique_ptr<lifetime_manager> lifetime_;
    };
} // namespace avis::rhi::d3d12

#endif
