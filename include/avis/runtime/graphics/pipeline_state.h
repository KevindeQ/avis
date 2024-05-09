#ifndef RUNTIME_GRAPHICS_PIPELINE_STATE_H
#define RUNTIME_GRAPHICS_PIPELINE_STATE_H

#include "avis/core/common.h"
#include "avis/runtime/graphics/graphics_device.h"
#include "avis/runtime/graphics/root_signature.h"
#include "avis/runtime/graphics/shaders.h"

namespace graphics
{
    class stream_output_descriptor
    {};

    class blend_descriptor
    {
    public:
    private:
        bool enable_alpha_to_coverage;
        bool enable_independent_blend;
    };

    class rasterizer_descriptor
    {
    public:
    private:
        fill_mode f;
        cull_mode c;
        bool front_counter_clockwise;
        std::int32_t depth_bias;
        float depth_bias_clamp;
        float slope_scaled_depth_bias;
        bool enable_depth_clip;
        bool enable_multi_sample;
        bool enable_antialised_line;
        std::uint32_t forced_sample_count;
        conservative_rasterization_mode conservative_raster;
    };

    class depth_stencil_operation_descriptor
    {
    public:
    private:
        depth_stencil_operation stencil_fail_op;
        depth_stencil_operation stencil_depth_fail_op;
        depth_stencil_operation stencil_pass_op;
        comparison_function stencil_function;
    };

    class depth_stencil_descriptor
    {
    public:
    private:
        bool enable_depth;
        depth_write_mask mask;
        bool enable_stencil;
        std::uint8_t stencil_read_mask;
        std::uint8_t stencil_write_mask;
        depth_stencil_operation_descriptor front_face;
        depth_stencil_operation_descriptor back_face;
    };

    class input_layout_descriptor
    {
    public:
    private:
    };

    class sample_descriptor
    {
    public:
    private:
        std::uint32_t count;
        std::uint32_t quality;
    };

    //class graphics_pipeline_state_descriptor
    //{
    //public:
    //private:
    //    root_signature_ref root_signature;
    //    shader_ref vertex_shader;
    //    shader_ref domain_shader;
    //    shader_ref hull_shader;
    //    shader_ref geometry_shader;
    //    shader_ref pixel_shader;
    //    // stream_output_descriptor stream_output;
    //    // blend_descriptor blend_state;
    //    std::uint32_t smaple_mask;
    //    rasterizer_descriptor rasterizer_state;
    //    depth_stencil_descriptor depth_stencil_state;
    //    input_layout_descriptor input_layout;
    //    index_buffer_strip_cut_value strip_cut_value;
    //    primitive_topology_type primitive_topology;
    //    std::vector<format_descriptor> render_targets;
    //    format_descriptor dsv_format;
    //    sample_descriptor sample_state;
    //    pipeline_state_flags state_flags;
    //};

    class pipeline_state
    {
    public:
        pipeline_state();
        pipeline_state(com_ptr<ID3D12PipelineState> state);

    private:
        com_ptr<ID3D12PipelineState> native_pipeline_state;
    };

    class graphics_pipeline_state_builder
    {
    public:
        graphics_pipeline_state_builder();
        graphics_pipeline_state_builder(const graphics_pipeline_state_builder&) = delete;
        graphics_pipeline_state_builder(graphics_pipeline_state_builder&&) = delete;

        const graphics_pipeline_state_builder& operator=(const graphics_pipeline_state_builder&) = delete;
        const graphics_pipeline_state_builder& operator=(graphics_pipeline_state_builder&&) = delete;

        /*pipeline_state create_pipeline_state(const graphics_pipeline_state_descriptor& desc);
        pipeline_state create_pipeline_state(const compute_pipeline_state_descriptor& desc);*/

        pipeline_state build(graphics_device& device);

    private:
        D3D12_GRAPHICS_PIPELINE_STATE_DESC descriptor;
    };

    class compute_pipeline_state_builder
    {
    public:
        compute_pipeline_state_builder();
        compute_pipeline_state_builder(const compute_pipeline_state_builder&) = delete;
        compute_pipeline_state_builder(compute_pipeline_state_builder&&) = delete;

        const compute_pipeline_state_builder& operator=(const compute_pipeline_state_builder&) = delete;
        const compute_pipeline_state_builder& operator=(compute_pipeline_state_builder&&) = delete;

        compute_pipeline_state_builder& with_root_signature(const root_signature& signature);
        compute_pipeline_state_builder& with_compute_shader(const shader_ref& compute_shader);

        pipeline_state build(graphics_device& device);

    private:
        D3D12_COMPUTE_PIPELINE_STATE_DESC descriptor;
    };
} // namespace graphics

#endif
