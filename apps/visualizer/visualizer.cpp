#include "visualizer.h"

#include "avis/middleware/data_formats/ply/ply_parser.h"
#include "avis/middleware/input/input_device_keyboard.h"
#include "avis/middleware/input/input_device_mouse.h"
#include "avis/middleware/input/input_state.h"
#include "avis/middleware/runtime.h"

visualizer::visualizer(basic_app_config& config) :
    basic_app(config),

    render_window{ { L"D3D12 Demo", 800, 800 } },

    viewport{ 0.0f,
              0.0f,
              static_cast<float>(render_window.width()),
              static_cast<float>(render_window.height()),
              D3D12_MIN_DEPTH,
              D3D12_MAX_DEPTH },
    scissor_rect{ 0, 0, static_cast<long>(render_window.width()), static_cast<long>(render_window.height()) },

    constant_buffer_data{},
    constant_buffer_data_begin{ nullptr },

    global_camera{ },
    global_camera_controller{ },

    threads{ 3 },
    file_load_service{ threads, 3 },

    current_inputs{},
    input_decoder{},
    global_input_context{},
    movement_input_context{}
{
    configure_rendering(false);
    configure_window_resize();
    configure_input();

    load_assets();

    load_content();
}

visualizer::~visualizer()
{
    // Ensure that the GPU is no longer referencing resources that are about to be
    // cleaned up by the destructor.
    wait_for_gpu();

    CloseHandle(fence_event);
}

void visualizer::on_update(const step_timer& timer)
{
    visualizer_input_state current_inputs = input_decoder.snapshot_inputs();
    input_decoder.clear_inputs();

    if (current_inputs.contains(input_actions::exit_app))
    {
        render_window.close();
    }

    if (current_inputs.contains(input_actions::reset_camera))
    {
        global_camera_controller.reset();
    }

    if (current_inputs.contains(input_states::camera_move_forward))
    {
        global_camera_controller.move_forward(1.0f, timer.elapsed_seconds());
    }
    if (current_inputs.contains(input_states::camera_move_backward))
    {
        global_camera_controller.move_forward(-1.0f, timer.elapsed_seconds());
    }
    if (current_inputs.contains(input_states::camera_move_left))
    {
        global_camera_controller.move_left(1.0f, timer.elapsed_seconds());
    }
    if (current_inputs.contains(input_states::camera_move_right))
    {
        global_camera_controller.move_left(-1.0f, timer.elapsed_seconds());
    }

    if (current_inputs.contains(input_ranges::camera_rotate_pitch))
    {
        // TODO: Replace amount with actual value from input
        global_camera_controller.rotate_pitch(1.0f, timer.elapsed_seconds());
    }
    if (current_inputs.contains(input_ranges::camera_rotate_yaw))
    {
        // TODO: Replace amount with actual value from input
        global_camera_controller.rotate_yaw(1.0f, timer.elapsed_seconds());
    }

    global_camera_controller.update();

    // Update shader data
    Eigen::Matrix4f::Map(constant_buffer_data.matrix_view_projection.data()) = global_camera.view_projection_matrix();
    std::memcpy(constant_buffer_data_begin, &constant_buffer_data, sizeof(constant_buffer_data));
}

void visualizer::on_render()
{
    // Command list allocators can only be reset when the associated
    // command lists have finished execution on the GPU; apps should use
    // fences to determine GPU execution progress.
    throw_if_failed(command_allocators[frame_index]->Reset());

    // However, when ExecuteCommandList() is called on a particular command
    // list, that command list can then be reset at any time and must be before
    // re-recording.
    throw_if_failed(command_list->Reset(command_allocators[frame_index].get(), pipeline_state.get()));

    // Set necessary state.
    command_list->SetGraphicsRootSignature(root_signature.get());

    std::array<ID3D12DescriptorHeap*, 1> descriptor_heaps{ { heap_cbv.get() } };
    command_list->SetDescriptorHeaps(descriptor_heaps.size(), descriptor_heaps.data());
    command_list->SetGraphicsRootDescriptorTable(0, heap_cbv->GetGPUDescriptorHandleForHeapStart());

    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    // Indicate that the back buffer will be used as a render target.
    D3D12_RESOURCE_BARRIER barrier_render_target{};
    barrier_render_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_render_target.Transition.pResource = render_targets[frame_index].get();
    barrier_render_target.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier_render_target.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier_render_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier_render_target.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    command_list->ResourceBarrier(1, &barrier_render_target);

    D3D12_CPU_DESCRIPTOR_HANDLE handle_rtv{};
    handle_rtv.ptr = heap_rtv->GetCPUDescriptorHandleForHeapStart().ptr +
                     static_cast<std::int64_t>(frame_index) * static_cast<std::uint64_t>(descriptor_size_rtv);
    command_list->OMSetRenderTargets(1, &handle_rtv, FALSE, nullptr);

    // Record commands.
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    command_list->ClearRenderTargetView(handle_rtv, clearColor, 0, nullptr);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0, 1, &vertex_buffer_view);
    command_list->DrawInstanced(3, 1, 0, 0);

    // Indicate that the back buffer will now be used to present.
    D3D12_RESOURCE_BARRIER barrier_render_present{};
    barrier_render_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_render_present.Transition.pResource = render_targets[frame_index].get();
    barrier_render_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier_render_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier_render_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier_render_present.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    command_list->ResourceBarrier(1, &barrier_render_present);

    throw_if_failed(command_list->Close());

    // Execute the command list.
    ID3D12CommandList* ppCommandLists[] = { command_list.get() };
    command_queue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Present the frame.
    throw_if_failed(swap_chain->Present(1, 0));

    move_to_next_frame();
}

void visualizer::load_content()
{
    /*io::file_descriptor test_file =
        file_context.create_descriptor("E:\\Projects\\D3D12TechDemo\\data\\models\\buddha\\buddha.obj");*/
    /*std::future<file_output> test_file_contents = file_load_service.async_read_file<file_output>(
        test_file, [](const streams::memory_stream & data) { return file_output{}; });*/
    /*std::future<file_output> test_file_contents =
        file_load_service.async_read_file<file_output>(test_file, file_parser{});*/

    io::file_descriptor ply_file =
        file_context.create_descriptor("E:\\Data\\KITTI-360\\data_3d_semantics\\train\\2013_05_28_drive_0000_"
                                       "sync\\dynamic\\0000000002_0000000385.ply");
    std::future<geometry::data_store> test_file_contents = file_load_service.async_read_file<geometry::data_store>(
        ply_file,
        [](const streams::memory_stream& data)
        {
            data_formats::ply::ply_parser parser;
            return parser.parse(data);
        });
    geometry::data_store geometry_data = test_file_contents.get();

    /*io::basic_file_descriptor database_file{ "E:\\Projects\\avis\\assets\\maps\\point_cloud.adf" };
    io::basic_file_descriptor index_file{ "E:\\Projects\\avis\\assets\\maps\\point_cloud.aif" };
    data::point_cloud database{ index_file, database_file };
    database.fetch_data(Eigen::Vector3f{});*/

    global_camera = camera{ std::numbers::pi / 4.0f,
                            static_cast<float>(render_window.width()) / static_cast<float>(render_window.height()),
                            0.1f,
                            100.0f };
    global_camera.look_at(Eigen::Vector3f{ 0.0f, 0.0f, 2.0f }, Eigen::Vector3f::Zero(), Eigen::Vector3f::UnitY());
    global_camera_controller = camera_controller{ &global_camera };
}

void visualizer::configure_window_resize()
{
    render_window.on_message(
        WM_SIZE,
        [this](WPARAM wparam, LPARAM lparam)
        {
            on_window_resize(render_window.width(), render_window.height(), wparam == SIZE_MINIMIZED);
            return 0;
        });
}

void visualizer::on_window_resize(const std::uint32_t width, const std::uint32_t height, const bool minimized)
{
    wait_for_gpu();

    for (std::uint32_t index = 0; index < frame_count; ++index)
    {
        render_targets[index].reset();
        fence_values[index] = fence_values[frame_index];
    }

    DXGI_SWAP_CHAIN_DESC descriptor = {};
    swap_chain->GetDesc(&descriptor);
    throw_if_failed(
        swap_chain->ResizeBuffers(frame_count, width, height, descriptor.BufferDesc.Format, descriptor.Flags));

    frame_index = swap_chain->GetCurrentBackBufferIndex();

    load_size_dependent_resources();
}

void visualizer::configure_rendering(const bool use_warp_device)
{
    std::uint32_t dxgi_factory_flags = 0;

#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    // NOTE: Enabling the debug layer after device creation will invalidate the active device.
    {
        com_ptr<ID3D12Debug> debug_controller;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller))))
        {
            debug_controller->EnableDebugLayer();

            // Enable additional debug layers.
            dxgi_factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    com_ptr<IDXGIFactory4> factory;
    throw_if_failed(CreateDXGIFactory2(dxgi_factory_flags, IID_PPV_ARGS(&factory)));

    if (use_warp_device)
    {
        com_ptr<IDXGIAdapter> warp_adapter;
        throw_if_failed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter)));

        throw_if_failed(D3D12CreateDevice(warp_adapter.get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&graphics_device)));
    }
    else
    {
        com_ptr<IDXGIAdapter1> hardware_adapter;
        get_hardware_adapter(factory, hardware_adapter, true);

        throw_if_failed(
            D3D12CreateDevice(hardware_adapter.get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&graphics_device)));
    }

    // Describe and create the command queue.
    D3D12_COMMAND_QUEUE_DESC queue_descriptor = {};
    queue_descriptor.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_descriptor.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    throw_if_failed(graphics_device->CreateCommandQueue(&queue_descriptor, IID_PPV_ARGS(&command_queue)));

    // Describe and create the swap chain.
    DXGI_SWAP_CHAIN_DESC1 swap_chain_descriptor = {};
    swap_chain_descriptor.BufferCount = frame_count;
    swap_chain_descriptor.Width = render_window.width();
    swap_chain_descriptor.Height = render_window.height();
    swap_chain_descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_descriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_descriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_descriptor.SampleDesc.Count = 1;

    com_ptr<IDXGISwapChain1> swap_chain_temp;
    throw_if_failed(factory->CreateSwapChainForHwnd(
        command_queue.get(), // Swap chain needs the queue so that it can force a flush on it.
        render_window.native_handle(),
        &swap_chain_descriptor,
        nullptr,
        nullptr,
        &swap_chain_temp));

    // This sample does not support fullscreen transitions.
    throw_if_failed(factory->MakeWindowAssociation(render_window.native_handle(), DXGI_MWA_NO_ALT_ENTER));

    throw_if_failed(swap_chain_temp.as(swap_chain));
    frame_index = swap_chain->GetCurrentBackBufferIndex();

    // Create descriptor heaps.
    {
        // Describe and create a render target view (RTV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC heap_rtv_descriptor{};
        heap_rtv_descriptor.NumDescriptors = frame_count;
        heap_rtv_descriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_rtv_descriptor.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        throw_if_failed(graphics_device->CreateDescriptorHeap(&heap_rtv_descriptor, IID_PPV_ARGS(&heap_rtv)));

        descriptor_size_rtv = graphics_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC heap_cbv_descriptor{};
        heap_cbv_descriptor.NumDescriptors = 1;
        heap_cbv_descriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_cbv_descriptor.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        throw_if_failed(graphics_device->CreateDescriptorHeap(&heap_cbv_descriptor, IID_PPV_ARGS(&heap_cbv)));
    }

    // Create frame resources.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle_rtv(heap_rtv->GetCPUDescriptorHandleForHeapStart());

        // Create a RTV and a command allocator for each frame.
        for (std::uint32_t index = 0; index < frame_count; index++)
        {
            throw_if_failed(swap_chain->GetBuffer(index, IID_PPV_ARGS(&render_targets[index])));
            graphics_device->CreateRenderTargetView(render_targets[index].get(), nullptr, handle_rtv);
            handle_rtv.ptr += descriptor_size_rtv;

            throw_if_failed(graphics_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators[index])));
        }
    }
}

void visualizer::get_hardware_adapter(
    com_ptr<IDXGIFactory4>& factory, com_ptr<IDXGIAdapter1>& adapter, bool request_high_performance_adapter)
{
    adapter.reset();

    com_ptr<IDXGIAdapter1> adapter_temp;

    com_ptr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6))))
    {
        for (std::uint32_t adapter_index = 0; SUCCEEDED(factory6->EnumAdapterByGpuPreference(
                 adapter_index,
                 request_high_performance_adapter == true ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE :
                                                            DXGI_GPU_PREFERENCE_UNSPECIFIED,
                 IID_PPV_ARGS(&adapter_temp)));
             ++adapter_index)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter_temp->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                // Don't select the Basic Render Driver adapter.
                // If you want a software adapter, pass in "/warp" on the command line.
                continue;
            }

            // Check to see whether the adapter supports Direct3D 12, but don't create the
            // actual device yet.
            if (SUCCEEDED(
                    D3D12CreateDevice(adapter_temp.get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
        }
    }

    if (adapter_temp.get() == nullptr)
    {
        for (UINT adapterIndex = 0; SUCCEEDED(factory->EnumAdapters1(adapterIndex, &adapter_temp)); ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter_temp->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                // Don't select the Basic Render Driver adapter.
                // If you want a software adapter, pass in "/warp" on the command line.
                continue;
            }

            // Check to see whether the adapter supports Direct3D 12, but don't create the
            // actual device yet.
            if (SUCCEEDED(
                    D3D12CreateDevice(adapter_temp.get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
        }
    }

    adapter = adapter_temp;
}

void visualizer::wait_for_gpu()
{
    // Schedule a Signal command in the queue.
    throw_if_failed(command_queue->Signal(fence.get(), fence_values[frame_index]));

    // Wait until the fence has been processed.
    throw_if_failed(fence->SetEventOnCompletion(fence_values[frame_index], fence_event));
    WaitForSingleObjectEx(fence_event, INFINITE, FALSE);

    // Increment the fence value for the current frame.
    fence_values[frame_index]++;
}

void visualizer::move_to_next_frame()
{
    // Schedule a Signal command in the queue.
    const std::uint64_t current_fence_value = fence_values[frame_index];
    throw_if_failed(command_queue->Signal(fence.get(), current_fence_value));

    // Update the frame index.
    frame_index = swap_chain->GetCurrentBackBufferIndex();

    // If the next frame is not ready to be rendered yet, wait until it is ready.
    if (fence->GetCompletedValue() < fence_values[frame_index])
    {
        throw_if_failed(fence->SetEventOnCompletion(fence_values[frame_index], fence_event));
        WaitForSingleObjectEx(fence_event, INFINITE, FALSE);
    }

    // Set the fence value for the next frame.
    fence_values[frame_index] = current_fence_value + 1;
}

void visualizer::load_assets()
{
    // Create an empty root signature.
    {
        std::array<D3D12_DESCRIPTOR_RANGE1, 1> ranges;
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;

        std::array<D3D12_ROOT_PARAMETER1, 1> root_parameters{};
        root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_parameters[0].DescriptorTable.pDescriptorRanges = ranges.data();
        root_parameters[0].DescriptorTable.NumDescriptorRanges = ranges.size();
        root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_signature_descriptor{};
        root_signature_descriptor.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        root_signature_descriptor.Desc_1_1.NumParameters = root_parameters.size();
        root_signature_descriptor.Desc_1_1.pParameters = root_parameters.data();
        root_signature_descriptor.Desc_1_1.NumStaticSamplers = 0;
        root_signature_descriptor.Desc_1_1.pStaticSamplers = nullptr;
        root_signature_descriptor.Desc_1_1.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS /*|
                                                   D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                                   D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED*/
            ;

        com_ptr<ID3DBlob> signature;
        com_ptr<ID3DBlob> error;
        throw_if_failed(D3D12SerializeVersionedRootSignature(
            &root_signature_descriptor, signature.address_of(), error.address_of()));
        throw_if_failed(graphics_device->CreateRootSignature(
            0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root_signature)));
    }

    // Create the pipeline state, which includes compiling and loading shaders.
    {
        com_ptr<ID3DBlob> vertexShader;
        com_ptr<ID3DBlob> pixelShader;

        io::file_descriptor file_cso_vs = file_context.create_descriptor(".\\assets\\cso\\triangle_vs.cso");
        std::future<std::vector<unsigned char>> shader_ref_vs =
            file_load_service.async_read_file<std::vector<unsigned char>>(
                file_cso_vs,
                [&](const streams::memory_stream& stream)
                {
                    std::span<unsigned char> bytes = stream.data();
                    return std::vector<unsigned char>{ bytes.begin(), bytes.end() };
                });
        std::vector<unsigned char> bytes_vs = shader_ref_vs.get();

        D3D12_SHADER_BYTECODE byte_code_vs{};
        byte_code_vs.pShaderBytecode = bytes_vs.data();
        byte_code_vs.BytecodeLength = bytes_vs.size();

        io::file_descriptor file_cso_ps = file_context.create_descriptor(".\\assets\\cso\\triangle_ps.cso");
        std::future<std::vector<unsigned char>> shader_ref_ps =
            file_load_service.async_read_file<std::vector<unsigned char>>(
                file_cso_ps,
                [&](const streams::memory_stream& stream)
                {
                    std::span<unsigned char> bytes = stream.data();
                    return std::vector<unsigned char>{ bytes.begin(), bytes.end() };
                });
        std::vector<unsigned char> bytes_ps = shader_ref_ps.get();

        D3D12_SHADER_BYTECODE byte_code_ps{};
        byte_code_ps.pShaderBytecode = bytes_ps.data();
        byte_code_ps.BytecodeLength = bytes_ps.size();

        // Define the vertex input layout.
        D3D12_INPUT_ELEMENT_DESC input_element_descs[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        // Describe and create the graphics pipeline state object (PSO).
        D3D12_RASTERIZER_DESC rasterizer_descriptor{};
        rasterizer_descriptor.FillMode = D3D12_FILL_MODE_SOLID;
        rasterizer_descriptor.CullMode = D3D12_CULL_MODE_BACK;
        rasterizer_descriptor.FrontCounterClockwise = TRUE;
        rasterizer_descriptor.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        rasterizer_descriptor.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        rasterizer_descriptor.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        rasterizer_descriptor.DepthClipEnable = TRUE;
        rasterizer_descriptor.MultisampleEnable = FALSE;
        rasterizer_descriptor.AntialiasedLineEnable = FALSE;
        rasterizer_descriptor.ForcedSampleCount = 0;
        rasterizer_descriptor.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

        D3D12_BLEND_DESC blend_descriptor{};
        blend_descriptor.AlphaToCoverageEnable = FALSE;
        blend_descriptor.IndependentBlendEnable = FALSE;
        const D3D12_RENDER_TARGET_BLEND_DESC default_render_target_blend_descriptor = {
            FALSE,
            FALSE,
            D3D12_BLEND_ONE,
            D3D12_BLEND_ZERO,
            D3D12_BLEND_OP_ADD,
            D3D12_BLEND_ONE,
            D3D12_BLEND_ZERO,
            D3D12_BLEND_OP_ADD,
            D3D12_LOGIC_OP_NOOP,
            D3D12_COLOR_WRITE_ENABLE_ALL,
        };
        for (UINT index = 0; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index)
        {
            blend_descriptor.RenderTarget[index] = default_render_target_blend_descriptor;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_descriptor = {};
        pso_descriptor.InputLayout = { input_element_descs, _countof(input_element_descs) };
        pso_descriptor.pRootSignature = root_signature.get();
        pso_descriptor.VS = byte_code_vs;
        pso_descriptor.PS = byte_code_ps;
        pso_descriptor.RasterizerState = rasterizer_descriptor;
        pso_descriptor.BlendState = blend_descriptor;
        pso_descriptor.DepthStencilState.DepthEnable = FALSE;
        pso_descriptor.DepthStencilState.StencilEnable = FALSE;
        pso_descriptor.SampleMask = UINT_MAX;
        pso_descriptor.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_descriptor.NumRenderTargets = 1;
        pso_descriptor.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_descriptor.SampleDesc.Count = 1;
        throw_if_failed(graphics_device->CreateGraphicsPipelineState(&pso_descriptor, IID_PPV_ARGS(&pipeline_state)));
    }

    // Create the command list.
    throw_if_failed(graphics_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        command_allocators[frame_index].get(),
        pipeline_state.get(),
        IID_PPV_ARGS(&command_list)));

    // Command lists are created in the recording state, but there is nothing
    // to record yet. The main loop expects it to be closed, so close it now.
    throw_if_failed(command_list->Close());

    // Create the vertex buffer.
    {
        // Define the geometry for a triangle.
        float aspect_ratio = static_cast<float>(render_window.width()) / static_cast<float>(render_window.height());
        Vertex triangle_vertices[] = { { { 0.0f, 0.25f * aspect_ratio, 0.5f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
                                       { { -0.25f, -0.25f * aspect_ratio, 0.5f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
                                       { { 0.25f, -0.25f * aspect_ratio, 0.5f }, { 0.0f, 0.0f, 1.0f, 1.0f } } };

        const std::uint32_t vertex_buffer_size = sizeof(triangle_vertices);

        D3D12_HEAP_PROPERTIES heap_properties{};
        heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heap_properties.CreationNodeMask = 1;
        heap_properties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC resource_descriptor{};
        resource_descriptor.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource_descriptor.Alignment = 0;
        resource_descriptor.Width = vertex_buffer_size;
        resource_descriptor.Height = 1;
        resource_descriptor.DepthOrArraySize = 1;
        resource_descriptor.MipLevels = 1;
        resource_descriptor.Format = DXGI_FORMAT_UNKNOWN;
        resource_descriptor.SampleDesc.Count = 1;
        resource_descriptor.SampleDesc.Quality = 0;
        resource_descriptor.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resource_descriptor.Flags = D3D12_RESOURCE_FLAG_NONE;

        // Note: using upload heaps to transfer static data like vert buffers is not
        // recommended. Every time the GPU needs it, the upload heap will be marshalled
        // over. Please read up on Default Heap usage. An upload heap is used here for
        // code simplicity and because there are very few verts to actually transfer.
        throw_if_failed(graphics_device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_NONE,
            &resource_descriptor,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&vertex_buffer)));

        D3D12_RANGE read_range{}; // We do not intend to read from this resource on the CPU.
        read_range.Begin = 0;
        read_range.End = 0;

        // Copy the triangle data to the vertex buffer.
        std::uint8_t* vertex_data_begin;
        throw_if_failed(vertex_buffer->Map(0, &read_range, reinterpret_cast<void**>(&vertex_data_begin)));
        memcpy(vertex_data_begin, triangle_vertices, sizeof(triangle_vertices));
        vertex_buffer->Unmap(0, nullptr);

        // Initialize the vertex buffer view.
        vertex_buffer_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
        vertex_buffer_view.StrideInBytes = sizeof(Vertex);
        vertex_buffer_view.SizeInBytes = vertex_buffer_size;
    }

    // Creae the constant buffer
    {
        const std::uint32_t buffer_size_constant_buffer = sizeof(constant_buffer_scene);

        D3D12_HEAP_PROPERTIES properties_heap{};
        properties_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        properties_heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties_heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties_heap.CreationNodeMask = 1;
        properties_heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC resource_descriptor{};
        resource_descriptor.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource_descriptor.Alignment = 0;
        resource_descriptor.Width = buffer_size_constant_buffer;
        resource_descriptor.Height = 1;
        resource_descriptor.DepthOrArraySize = 1;
        resource_descriptor.MipLevels = 1;
        resource_descriptor.Format = DXGI_FORMAT_UNKNOWN;
        resource_descriptor.SampleDesc.Count = 1;
        resource_descriptor.SampleDesc.Quality = 0;
        resource_descriptor.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resource_descriptor.Flags = D3D12_RESOURCE_FLAG_NONE;

        throw_if_failed(graphics_device->CreateCommittedResource(
            &properties_heap,
            D3D12_HEAP_FLAG_NONE,
            &resource_descriptor,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&constant_buffer)));

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_descriptor{};
        cbv_descriptor.BufferLocation = constant_buffer->GetGPUVirtualAddress();
        cbv_descriptor.SizeInBytes = buffer_size_constant_buffer;
        graphics_device->CreateConstantBufferView(&cbv_descriptor, heap_cbv->GetCPUDescriptorHandleForHeapStart());

        // Map and initialize the constant buffer. This resource isn't unmapped until the app closes
        D3D12_RANGE range_read{};
        range_read.Begin = 0;
        range_read.End = 0;
        throw_if_failed(constant_buffer->Map(0, &range_read, reinterpret_cast<void**>(&constant_buffer_data_begin)));
        std::memcpy(constant_buffer_data_begin, &constant_buffer_data, sizeof(constant_buffer_data));
    }

    // Create synchronization objects and wait until assets have been uploaded to the GPU.
    {
        throw_if_failed(
            graphics_device->CreateFence(fence_values[frame_index], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        fence_values[frame_index]++;

        // Create an event handle to use for frame synchronization.
        fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr)
        {
            throw_if_failed(HRESULT_FROM_WIN32(GetLastError()));
        }

        // Wait for the command list to execute; we are reusing the same command
        // list in our main loop but for now, we just want to wait for setup to
        // complete before continuing.
        wait_for_gpu();
    }
}

void visualizer::load_size_dependent_resources()
{
    update_viewport_and_scissor();

    D3D12_CPU_DESCRIPTOR_HANDLE handle_rtv{};
    handle_rtv.ptr = heap_rtv->GetCPUDescriptorHandleForHeapStart().ptr;

    for (std::uint32_t index = 0; index < frame_count; ++index)
    {
        throw_if_failed(swap_chain->GetBuffer(index, IID_PPV_ARGS(&render_targets[index])));
        graphics_device->CreateRenderTargetView(render_targets[index].get(), nullptr, handle_rtv);
        handle_rtv.ptr += descriptor_size_rtv;
    }
}

void visualizer::update_viewport_and_scissor()
{
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = render_window.width();
    viewport.Height = render_window.height();
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;

    scissor_rect.left = 0;
    scissor_rect.right = render_window.width();
    scissor_rect.top = 0;
    scissor_rect.bottom = render_window.height();
}

void visualizer::configure_input()
{
    global_input_context.add_mapping(input::key_code::key_escape, input_actions::exit_app);

    global_input_context.add_mapping(input::key_code::key_r, input_actions::reset_camera);
    input_decoder.push_context(global_input_context);

    movement_input_context.add_mapping(input::key_code::key_up_arrow, input_states::camera_move_forward);
    movement_input_context.add_mapping(input::key_code::key_left_arrow, input_states::camera_move_left);
    movement_input_context.add_mapping(input::key_code::key_down_arrow, input_states::camera_move_backward);
    movement_input_context.add_mapping(input::key_code::key_right_arrow, input_states::camera_move_right);

    movement_input_context.add_mapping(input::key_code::key_w, input_states::camera_move_forward);
    movement_input_context.add_mapping(input::key_code::key_a, input_states::camera_move_left);
    movement_input_context.add_mapping(input::key_code::key_s, input_states::camera_move_backward);
    movement_input_context.add_mapping(input::key_code::key_d, input_states::camera_move_right);
    input_decoder.push_context(movement_input_context);

    input_decoder.register_device<input::input_device_keyboard>(render_window.native_handle());
    input_decoder.register_device<input::input_device_mouse>(render_window.native_handle());

    render_window.on_message(
        WM_INPUT,
        [this](WPARAM wparam, LPARAM lparam)
        {
            // Check if this message can be ignored because the window was out of focus
            if (GET_RAWINPUT_CODE_WPARAM(wparam) == RIM_INPUTSINK)
            {
                return 0ll;
            }

            input_decoder.decode(wparam, lparam);

            // According to MSDN, DefWindowProc must be called to give the system a chance to perform
            // cleanup. See https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-input for more
            // info.
            return DefWindowProcW(render_window.native_handle(), WM_INPUT, wparam, lparam);
        });
}

int __stdcall wWinMain(
    _In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd)
{
    runtime_builder engine_builder;
    engine_builder.configure_engine(
        [](configuration_builder& builder)
        {
            // builder.use_root_path("");
            // builder.configure_from_json("E:\\Projects\\D3D12TechDemo\\build\\bin\\Debug\\test.txt");
            // builder.configure_from_ini("");
            // builder.configure_from_commandline();
        });
    engine_builder.configure_logging(
        [](logging_builder& builder)
        {
            // builder.add_log();
            // builder.add_log();
        });
    runtime engine = engine_builder.build();

    basic_app_config configuration;
    configuration.fixed_time_delta = 16ms;

    return engine.execute<visualizer>(configuration);
}

// file_output file_parser::operator()(const streams::memory_stream& data)
//{
//     return file_output{};
// }
