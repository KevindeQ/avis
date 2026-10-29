#include "avis/rhi/shared.h"

#include "avis/rhi/d3d12/deferred_release.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/d3d12/shared_objects.h"
#include "avis/rhi/render_hardware_interface.h"

#include <memory>
#include <utility>

namespace avis::rhi
{
    // ---- shared_fence ----

    shared_fence::shared_fence(d3d12::shared_fence_state* const state) noexcept : state_{ state } {}

    shared_fence::shared_fence(shared_fence&& other) noexcept : state_{ std::exchange(other.state_, nullptr) } {}

    shared_fence& shared_fence::operator=(shared_fence&& other) noexcept
    {
        if (this != &other)
        {
            release();
            state_ = std::exchange(other.state_, nullptr);
        }
        return *this;
    }

    shared_fence::~shared_fence()
    {
        release();
    }

    void shared_fence::release() noexcept
    {
        if (d3d12::shared_fence_state* const state = std::exchange(state_, nullptr))
        {
            // A queue may still signal or wait on it (§23.9); out of memory terminates (§23.8)
            state->lifetime->destroy(d3d12::release_entry{ std::unique_ptr<d3d12::shared_fence_state>{ state } });
        }
    }

    std::uint64_t shared_fence::completed_value() const noexcept
    {
        return state_->fence->GetCompletedValue();
    }

    // ---- sharing ----

    sharing::sharing(render_hardware_interface& hardware_interface) noexcept : state_{ &hardware_interface.state() } {}

    result<texture_handle> sharing::create_shared_texture(const texture_description& description)
    {
        return state_->resources().create_shared_texture(description);
    }

    result<shared_handle> sharing::export_shared_handle(const texture_handle texture)
    {
        auto handle = state_->resources().export_shared_handle(texture);
        if (!handle)
        {
            return std::unexpected(std::move(handle.error()));
        }
        return shared_handle{ *handle };
    }

    result<shared_fence> sharing::create_shared_fence(const std::uint64_t initial_value)
    {
        auto fence = d3d12::create_fence(state_->device(), initial_value, "shared fence");
        if (!fence)
        {
            return std::unexpected(std::move(fence.error()));
        }
        (*fence)->lifetime = &state_->lifetime();
        return shared_fence{ fence->release() };
    }

    result<shared_fence> sharing::import_shared_fence(const shared_handle handle)
    {
        auto fence = d3d12::open_fence(state_->device(), handle.value);
        if (!fence)
        {
            return std::unexpected(std::move(fence.error()));
        }
        (*fence)->lifetime = &state_->lifetime();
        return shared_fence{ fence->release() };
    }

    result<shared_handle> sharing::export_shared_handle(const shared_fence& fence)
    {
        auto handle = d3d12::export_fence(state_->device(), *fence.state_);
        if (!handle)
        {
            return std::unexpected(std::move(handle.error()));
        }
        return shared_handle{ *handle };
    }

    gpu_wait_sender sharing::when_reached(const shared_fence& fence, const std::uint64_t value)
    {
        return gpu_wait_sender{ *state_, detail::gpu_wait_target{ .shared_fence = fence.state_, .shared_value = value } };
    }
} // namespace avis::rhi
