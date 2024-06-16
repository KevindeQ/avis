#ifndef VISUALIZER_H
#define VISUALIZER_H

#include "avis/middleware/basic_app.h"
#include "avis/middleware/geometry/data_store.h"
#include "avis/middleware/input/input_context.h"
#include "avis/middleware/input/input_dispatcher.h"
#include "avis/middleware/window.h"
#include "avis/runtime/data/point_cloud.h"
#include "avis/runtime/io/io_context.h"
#include "avis/runtime/parallel/thread_pool.h"

class sample_app : public basic_app
{
public:
    sample_app(basic_app_config& config);
    virtual ~sample_app();

    void on_update() override;

    void on_render() override;

private:
    void load_content();

    void configure_rendering(const bool use_warp_device);
    void get_hardware_adapter(
        com_ptr<IDXGIFactory4>& factory, com_ptr<IDXGIAdapter1>& adapter, bool request_high_performance_adapter);

    void wait_for_gpu();
    void move_to_next_frame();

    void load_assets();

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

    // Synchronization objects.
    std::uint32_t frame_index;
    HANDLE fence_event;
    com_ptr<ID3D12Fence> fence;
    std::array<std::uint64_t, frame_count> fence_values;

    parallel::thread_pool threads;
    io::io_context file_context;
    io::io_service file_load_service;

    input::input_dispatcher input_dispatcher;
    input::input_context global_input_context;
    input::input_context movement_input_context;
};

#endif
