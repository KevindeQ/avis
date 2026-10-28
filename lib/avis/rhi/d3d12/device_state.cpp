#include "avis/rhi/d3d12/device_state.h"

#include <utility>

namespace avis::rhi::d3d12
{
    result<std::unique_ptr<device_state>>
        device_state::create(const render_hardware_interface_configuration& configuration)
    {
        std::unique_ptr<device_state> state{ new device_state };

        auto created_device = device::create(configuration.adapter, configuration.debug);
        if (!created_device)
        {
            return std::unexpected(std::move(created_device.error()));
        }
        state->device_ = std::move(*created_device);
        return state;
    }

    device_state::~device_state()
    {
        try
        {
            wait_idle();
        }
        catch (...)
        {
            // A removed device has nothing left to wait for; the members are released either way
        }
    }

    d3d12::device& device_state::device() noexcept
    {
        return *device_;
    }

    std::size_t device_state::poll_completions() noexcept
    {
        return 0;
    }

    bool device_state::has_pending_work() const noexcept
    {
        return false;
    }

    void device_state::wait_idle() {}
} // namespace avis::rhi::d3d12
