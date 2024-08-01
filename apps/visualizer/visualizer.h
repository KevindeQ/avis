#ifndef VISUALIZER_H
#define VISUALIZER_H

#include "avis/middleware/basic_app.h"
#include "avis/middleware/geometry/data_store.h"
#include "avis/middleware/input/input_context.h"
#include "avis/middleware/input/input_decoder.h"
#include "avis/middleware/input/input_state.h"
#include "avis/middleware/window.h"
#include "avis/runtime/data/point_cloud.h"
#include "avis/runtime/io/io_context.h"
#include "avis/runtime/io/io_service.h"
#include "avis/runtime/parallel/thread_pool.h"
#include "camera.h"
#include "camera_controller.h"

enum class input_actions
{
    exit_app,

    reset_camera,
};

enum class input_states
{
    camera_move_forward,
    camera_move_backward,
    camera_move_left,
    camera_move_right
};

enum class input_ranges
{
    camera_move_forward_backward,
    camera_move_left_right,
    camera_rotate_pitch,
    camera_rotate_yaw
};

using visualizer_input_decoder = input::input_decoder<input_actions, input_states, input_ranges>;
using visualizer_input_state = input::input_state<input_actions, input_states, input_ranges>;
using visualizer_input_context = input::input_context<input_actions, input_states, input_ranges>;

struct alignas(256) constant_buffer_scene
{
public:
    std::array<float, 16> matrix_view_projection;
};

class visualizer : public basic_app
{
public:
    visualizer(basic_app_config& config);
    virtual ~visualizer();

    void on_update(const step_timer& timer) override;

    void on_render() override;

private:
    void load_content();

    void configure_window_resize();
    void on_window_resize(const std::uint32_t width, const std::uint32_t height, const bool minimized);

    void configure_rendering(const bool use_warp_device);
    void get_hardware_adapter(
        com_ptr<IDXGIFactory4>& factory, com_ptr<IDXGIAdapter1>& adapter, bool request_high_performance_adapter);

    void wait_for_gpu();
    void move_to_next_frame();

    void load_assets();
    void load_size_dependent_resources();
    void update_viewport_and_scissor();

    void configure_input();

private:
    struct float3
    {
        float x;
        float y;
        float z;
    };

    struct float4
    {
        float x;
        float y;
        float z;
        float w;
    };

    struct Vertex
    {
        float3 position;
        float4 color;
    };

    window render_window;

    static constexpr std::uint32_t frame_count = 3;

    com_ptr<ID3D12Device> graphics_device;
    std::array<com_ptr<ID3D12Resource>, frame_count> render_targets;
    std::array<com_ptr<ID3D12CommandAllocator>, frame_count> command_allocators;
    com_ptr<ID3D12CommandQueue> command_queue;
    com_ptr<ID3D12DescriptorHeap> heap_rtv;
    com_ptr<ID3D12DescriptorHeap> heap_cbv;
    com_ptr<ID3D12GraphicsCommandList> command_list;

    std::uint32_t descriptor_size_rtv;

    com_ptr<IDXGISwapChain3> swap_chain;

    com_ptr<ID3D12RootSignature> root_signature;
    com_ptr<ID3D12PipelineState> pipeline_state;

    // App resources.
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor_rect;

    com_ptr<ID3D12Resource> vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view;
    com_ptr<ID3D12Resource> constant_buffer;
    constant_buffer_scene constant_buffer_data;
    std::uint8_t* constant_buffer_data_begin;

    // Synchronization objects.
    std::uint32_t frame_index;
    HANDLE fence_event;
    com_ptr<ID3D12Fence> fence;
    std::array<std::uint64_t, frame_count> fence_values;

    camera global_camera;
    camera_controller global_camera_controller;

    parallel::thread_pool threads;
    io::io_context file_context;
    io::io_service file_load_service;

    visualizer_input_state current_inputs;
    visualizer_input_decoder input_decoder;
    visualizer_input_context global_input_context;
    visualizer_input_context movement_input_context;

    static constexpr double dead_zone_magnitude = 0.04f;
};

#endif
