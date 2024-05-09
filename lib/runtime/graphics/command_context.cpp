#include "avis/runtime/graphics/command_context.h"

namespace graphics
{
    copy_context copy_context::begin(const std::string name)
    {
        return copy_context{};
    }

    void compute_context::configure_pipeline(const pipeline_state new_state) {}

    void compute_context::dispatch(
        const std::uint32_t thread_group_count_x,
        const std::uint32_t thread_group_count_y,
        const std::uint32_t thread_group_count_z)
    {
        command_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z);
    }

    compute_context compute_context::begin(const std::string name)
    {
        return compute_context{};
    }

    void graphics_context::set_viewport(const viewport& port)
    {
        D3D12_VIEWPORT vp{};
        vp.TopLeftX = port.x;
        vp.TopLeftY = port.y;
        vp.Width = port.width;
        vp.Height = port.height;
        vp.MinDepth = 0.0;
        vp.MaxDepth = 1.0;

        // TODO: Must the pointer to D3D12_VIEWPORT be valid for the lifetime of the command_list?
        command_list->RSSetViewports(1, &vp);
    }

    void graphics_context::set_render_target(const render_target& target)
    {
        // TODO: Must the pointer to handle be valid for the lifetime of the command_list?
        command_list->OMSetRenderTargets(1, &target.handle, false, nullptr);
    }

    void graphics_context::draw_instanced(
        std::uint32_t vertex_count_per_instance,
        std::uint32_t instance_count,
        std::uint32_t start_vertex_location,
        std::uint32_t start_instance_location)
    {
        command_list->DrawInstanced(
            vertex_count_per_instance, instance_count, start_vertex_location, start_instance_location);
    }

    graphics_context graphics_context::begin(const std::string name)
    {
        return graphics_context{};
    }
} // namespace graphics
