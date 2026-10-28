#include "avis/rhi/memory.h"

#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/resource_manager.h"
#include "avis/rhi/render_hardware_interface.h"

namespace avis::rhi
{
    memory::memory(render_hardware_interface& hardware_interface) noexcept : state_{ &hardware_interface.state() } {}

    allocation_requirements memory::requirements(const texture_description& description) const
    {
        return state_->resources().requirements(description);
    }

    allocation_requirements memory::requirements(const buffer_description& description) const
    {
        return state_->resources().requirements(description);
    }

    result<heap_handle> memory::create_heap(const heap_description& description)
    {
        return state_->resources().create_heap(description);
    }

    void memory::destroy(const heap_handle heap) noexcept
    {
        state_->resources().destroy(heap);
    }

    result<texture_handle> memory::create_placed_texture(
        const heap_handle heap, const std::uint64_t offset, const texture_description& description)
    {
        return state_->resources().create_placed_texture(heap, offset, description);
    }

    result<buffer_handle> memory::create_placed_buffer(
        const heap_handle heap, const std::uint64_t offset, const buffer_description& description)
    {
        return state_->resources().create_placed_buffer(heap, offset, description);
    }
} // namespace avis::rhi
