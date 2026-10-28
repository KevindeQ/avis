#include "avis/rhi/completion_driver.h"

#include "avis/rhi/d3d12/completion_tracker.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/render_hardware_interface.h"

namespace avis::rhi
{
    completion_driver::completion_driver(render_hardware_interface& hardware_interface) noexcept :
        state_{ &hardware_interface.state() }
    {}

    std::size_t completion_driver::poll_completions() noexcept
    {
        return state_->poll_completions();
    }

    void* completion_driver::completion_event() const noexcept
    {
        return state_->completions().event();
    }

    bool completion_driver::has_pending_work() const noexcept
    {
        return state_->has_pending_work();
    }
} // namespace avis::rhi
