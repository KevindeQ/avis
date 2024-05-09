#ifndef RUNTIME_GRAPHICS_COMMAND_CONTEXT_H
#define RUNTIME_GRAPHICS_COMMAND_CONTEXT_H

#include "avis/core/common.h"
#include "avis/runtime/graphics/pipeline_state.h"

namespace graphics
{
    struct render_target
    {
    public:
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
    };

    struct viewport
    {
    public:
        float x;
        float y;

        float width;
        float height;
    };

    class command_context
    {
    public:
    protected:
        com_ptr<ID3D12GraphicsCommandList4> command_list;
    };

    class copy_context : public command_context
    {
    public:
        static copy_context begin(const std::string name = "");
    };

    class compute_context : public copy_context
    {
    public:
        void configure_pipeline(const pipeline_state new_state);

        void dispatch(
            const std::uint32_t thread_group_count_x,
            const std::uint32_t thread_group_count_y,
            const std::uint32_t thread_group_count_z);

        static compute_context begin(const std::string name = "");
    };

    class graphics_context : public compute_context
    {
    public:
        void set_viewport(const viewport& port);
        void set_render_target(const render_target& target);

        void draw_instanced(
            std::uint32_t vertex_count_per_instance,
            std::uint32_t instance_count,
            std::uint32_t start_vertex_location,
            std::uint32_t start_instance_location);

        static graphics_context begin(const std::string name = "");
    };
} // namespace graphics

#endif
