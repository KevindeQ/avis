#include "avis/rhi/render_hardware_interface.h"

#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/error.h"
#include "avis/threading/parallel_helpers.h"

#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>

namespace avis::rhi
{
    namespace
    {
        // D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2; tier 3 hardware may allow more, but Avis stays within
        // the limit every tier 3 device supports
        constexpr std::uint64_t maximum_shader_visible_descriptors = 1'000'000;

        // D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE
        constexpr std::uint32_t maximum_samplers = 2'048;

        // Step 1 of §20.3.2: the ranges of §4.1, before any device is created
        result<void> validate(const descriptor_heap_configuration& heaps)
        {
            const auto invalid = [](std::string message)
            { return std::unexpected(make_error(error_kind::invalid_argument, std::move(message))); };

            if (heaps.persistent_descriptor_count == 0)
            {
                return invalid(
                    "descriptor heaps: persistent_descriptor_count must be at least 1 (the null descriptor)");
            }
            const std::uint64_t shader_visible =
                std::uint64_t{ heaps.persistent_descriptor_count } + heaps.transient_descriptor_count;
            if (shader_visible > maximum_shader_visible_descriptors)
            {
                return invalid(
                    std::format(
                        "descriptor heaps: persistent_descriptor_count + transient_descriptor_count is {}, more than "
                        "one shader-visible heap holds ({})",
                        shader_visible,
                        maximum_shader_visible_descriptors));
            }
            if (heaps.sampler_count == 0 || heaps.sampler_count > maximum_samplers)
            {
                return invalid(
                    std::format(
                        "descriptor heaps: sampler_count is {}, it must be between 1 and {}",
                        heaps.sampler_count,
                        maximum_samplers));
            }
            if (heaps.render_target_view_count == 0)
            {
                return invalid("descriptor heaps: render_target_view_count must be at least 1");
            }
            if (heaps.depth_stencil_view_count == 0)
            {
                return invalid("descriptor heaps: depth_stencil_view_count must be at least 1");
            }
            return {};
        }
    } // namespace

    result<std::unique_ptr<render_hardware_interface>>
        render_hardware_interface::create(const render_hardware_interface_configuration& configuration)
    {
        if (auto valid = validate(configuration.descriptor_heaps); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }

        auto state = d3d12::device_state::create(configuration);
        if (!state)
        {
            return std::unexpected(std::move(state.error()));
        }
        return std::unique_ptr<render_hardware_interface>{ new render_hardware_interface{ std::move(*state) } };
    }

    render_hardware_interface::render_hardware_interface(std::unique_ptr<d3d12::device_state> state) noexcept :
        state_{ std::move(state) }
    {}

    render_hardware_interface::~render_hardware_interface() = default;

    const hardware_capabilities& render_hardware_interface::capabilities() const noexcept
    {
        return state_->device().capabilities();
    }

    void render_hardware_interface::wait_idle()
    {
        // Same per-thread check as sync_wait: a blocking wait on a parallel worker or inside a completion could
        // deadlock the work it waits for (§15)
        if (threading::detail::blocking_wait_forbidden())
        {
            throw std::logic_error{
                "render_hardware_interface::wait_idle: blocking waits are not allowed on this thread (a parallel "
                "worker, or inside a completion)"
            };
        }
        state_->wait_idle();
    }

    d3d12::device_state& render_hardware_interface::state() noexcept
    {
        return *state_;
    }
} // namespace avis::rhi
