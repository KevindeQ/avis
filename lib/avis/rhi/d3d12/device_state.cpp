#include "avis/rhi/d3d12/device_state.h"

#include "avis/rhi/d3d12/check.h"
#include "avis/rhi/d3d12/unique_handle.h"

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

        auto created_queues = device_queues::create(*state->device_);
        if (!created_queues)
        {
            return std::unexpected(std::move(created_queues.error()));
        }
        state->queues_ = std::move(*created_queues);

        auto created_tracker = completion_tracker::create(*state->device_, *state->queues_);
        if (!created_tracker)
        {
            return std::unexpected(std::move(created_tracker.error()));
        }
        state->completions_ = std::move(*created_tracker);

        auto created_descriptors = descriptor_heaps::create(*state->device_, configuration.descriptor_heaps);
        if (!created_descriptors)
        {
            return std::unexpected(std::move(created_descriptors.error()));
        }
        state->descriptors_ = std::move(*created_descriptors);

        auto created_lifetime = lifetime_manager::create(*state->queues_);
        if (!created_lifetime)
        {
            return std::unexpected(std::move(created_lifetime.error()));
        }
        state->lifetime_ = std::move(*created_lifetime);

        auto created_resources = resource_manager::create(*state->device_, *state->descriptors_, *state->lifetime_);
        if (!created_resources)
        {
            return std::unexpected(std::move(created_resources.error()));
        }
        state->resources_ = std::move(*created_resources);
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
        // Every release is freed while the subsystems its payloads point into still exist (§19.3)
        if (lifetime_)
        {
            lifetime_->drain();
        }
    }

    d3d12::device& device_state::device() noexcept
    {
        return *device_;
    }

    device_queues& device_state::queues() noexcept
    {
        return *queues_;
    }

    completion_tracker& device_state::completions() noexcept
    {
        return *completions_;
    }

    descriptor_heaps& device_state::descriptors() noexcept
    {
        return *descriptors_;
    }

    lifetime_manager& device_state::lifetime() noexcept
    {
        return *lifetime_;
    }

    resource_manager& device_state::resources() noexcept
    {
        return *resources_;
    }

    std::size_t device_state::poll_completions() noexcept
    {
        const std::size_t completed = completions_->process();
        if (lifetime_->housekeeping())
        {
            completions_->signal();
        }
        return completed;
    }

    bool device_state::has_pending_work() const noexcept
    {
        return completions_->has_waiters() || lifetime_->pending() > 0;
    }

    void device_state::wait_idle()
    {
        if (!lifetime_)
        {
            return; // creation failed part way: nothing was submitted
        }
        if (device_->lost())
        {
            device_->report_removed("wait_idle");
        }

        unique_handle event{ CreateEventW(nullptr, FALSE, FALSE, nullptr) };
        if (!event)
        {
            throw avis::runtime_error{ make_error(HRESULT_FROM_WIN32(GetLastError()), "wait_idle: CreateEvent") };
        }

        for (const queue_type type : { queue_type::graphics, queue_type::compute, queue_type::copy })
        {
            device_queue& queue = (*queues_)[type];
            const std::uint64_t target = queue.last_submitted();
            if (queue.is_complete(target))
            {
                continue;
            }
            // A removed device signals its fences to UINT64_MAX, so this wait ends then too
            if (auto armed = check_device(
                    *device_,
                    queue.fence().SetEventOnCompletion(target, event.get()),
                    "wait_idle: SetEventOnCompletion");
                !armed)
            {
                throw avis::runtime_error{ std::move(armed.error()) };
            }
            WaitForSingleObject(event.get(), INFINITE);
            static_cast<void>(queue.completed()); // marks the device lost on UINT64_MAX
            if (device_->lost())
            {
                device_->report_removed("wait_idle");
            }
        }
        poll_completions();
    }
} // namespace avis::rhi::d3d12
