# 09 — RHI (Render Hardware Interface)

## Code placement

| Feature | Namespace | CMake target | Location |
|---|---|---|---|
| Public RHI API (device, queues, command lists, resources, barriers, pipelines, …) | `avis::rhi` | `AVIS_LIB` | `include/avis/rhi/` |
| D3D12 implementation (private) | `avis::rhi::d3d12` (internal) | `AVIS_LIB` | `lib/avis/rhi/d3d12/` (headers not installed) |
| Root signature layout constants for shader authors (`rhi::root_signature_layout`) | `avis::rhi` | `AVIS_LIB` | `include/avis/rhi/root_signature_layout.h` |

## 1. Goals and constraints

| Requirement (idea.md) | Design response |
|---|---|
| DirectX 12 only, no Vulkan | One backend. No abstract backend interface or virtual dispatch. |
| No D3D12 types leak to users | Public headers declare only Avis types (handles, descs, enums). D3D12 headers are included only in `lib/avis/rhi/` sources and private headers (§2). |
| Renderer-agnostic | The RHI knows nothing about passes, G-buffers or materials. That belongs to the render graph and renderer. |
| Configurable frames in flight | `frame_configuration::frames_in_flight` (1–3, default 2) |
| Device management | `render_hardware_interface` (existing class, reworked): adapter selection (including by LUID for Qt interop), capability checks, debug configuration. COM lifetimes through the existing `com_ptr` (§3.1). |
| Command lists: allocation, recording, execution, tracking; **multi-threaded recording** | Per-thread, per-frame, per-queue allocator pools; timeline fences per queue |
| Async compute + copy queues | Three queues with cross-queue waits via synchronization points |
| PSO management (no pipeline libraries) | Hash-based PSO cache, thread-safe creation (parallelised by the caller) |
| Shaders compiled offline by DXC and loaded from file; no hot reload | Shader library loads `.cso` blobs through the IO layer |
| Bindless; **SM 6.6 required**, otherwise refuse to start | `ResourceDescriptorHeap` / `SamplerDescriptorHeap` indexing. Startup capability gate. |
| **Enhanced barriers**, without the Agility SDK | `ID3D12GraphicsCommandList7::Barrier` from the OS runtime, checked via `D3D12_OPTIONS12` |
| Resource allocation and lifetime | D3D12MemoryAllocator, generational handles, fence-based deferred destruction |
| Staging ring buffer for uploads | Persistently mapped upload ring consumed by the copy queue |
| Readback | Readback buffers with sender-based completion |
| Debug layer and GBV configurable | `debug_configuration`, plus info-queue routing and DRED |
| HDR output out of scope | The RHI doesn't own a swap chain at all (Qt presents, see [12](12-visualizer.md)) |

### 1.1 Platform baseline
- **Windows 11 24H2 (build 26100) or later.** Enhanced barriers without the Agility SDK need the inbox D3D12 runtime from that release. The startup check reports a clear message on older builds. This minimum is confirmed (see [99](99-decisions-and-open-questions.md), decision D-05).
- Windows SDK ≥ 10.0.26100 for the headers (`ID3D12Device10`, `ID3D12GraphicsCommandList7`, `D3D12_FEATURE_D3D12_OPTIONS12`).

## 2. Module layout

```
include/avis/rhi/                 # public, API-agnostic
  render_hardware_interface.h   render_hardware_interface, *_configuration, hardware_capabilities (existing file, reworked)
  queue.h             queue_type, queue<Queue>, synchronization_point, frame_token
  command_list.h      command_list<Queue> and its graphics/compute/copy views
  resources.h         texture_description, buffer_description, texture_handle, buffer_handle, sampler_handle
  formats.h           format enum + traits
  barriers.h          access_state, texture_barrier, buffer_barrier, global_barrier
  pipeline.h          graphics_pipeline_description, compute_pipeline_description, graphics_pipeline_handle, compute_pipeline_handle
  shader_library.h    shader_id, shader_library
  descriptors.h       bindless_index, descriptor regions (opaque)
  upload.h            upload_ring, upload_ticket
  readback.h          readback requests (senders)
  shared.h            shared_handle, shared_fence (cross-device interop)
  memory.h            heap_handle, placed resource creation (for render graph aliasing)
  gpu_profiler.h      timestamp queries per scope
lib/avis/rhi/
  d3d12/              all D3D12 code; private headers (device_state.h, conversions.h, …), all COM objects held in com_ptr
```

The existing `render_hardware_interface` class stays the root object of the RHI and is reworked to this design. Its public `native_device()` / `native_adapter()` accessors (which expose `ID3D12Device`) are removed. Native access is available only via the **private** header `lib/avis/rhi/d3d12/native_access.h`, for RHI internals and RHI unit tests. The existing `com_ptr.h` stays where it is (it's a generic template with no D3D12 include), but only private headers and sources use it.

## 3. Render hardware interface

The root object of the RHI is the **existing `avis::rhi::render_hardware_interface` class**. It's kept and reworked to match this design. In particular, its current public `native_device()` / `native_adapter()` accessors and the `<d3d12.h>` include in its header are removed (§2).

### 3.1 COM object lifetime
Every DirectX 12 and DXGI object is a COM interface. **All of them are owned through the existing `avis::rhi::com_ptr<T>`** (`include/avis/rhi/com_ptr.h`): devices, queues, fences, command allocators and lists, heaps, resources, PSOs, the root signature, debug and info-queue interfaces, and the DXGI factory and adapter.
- No raw `AddRef`/`Release` calls in RHI code. Ownership transfers by moving a `com_ptr`, and interface casts use `com_ptr::as<T>()` (`QueryInterface`).
- `com_ptr<ID3D12…>` members appear only in **private** headers and sources under `lib/avis/rhi/d3d12/`. The public slot maps (§5.2) hold private implementation records that contain the `com_ptr`s, so no COM type leaks into the public API.
- Deferred destruction (§5.2) works by moving the `com_ptr` into the release queue. The last `Release` happens when the GPU is done with the object.
- D3D12MemoryAllocator objects (`D3D12MA::Allocation`) are also COM-style reference counted, and are held through `com_ptr` as well.

### 3.2 Configuration
Creation is configured by **one configuration per concern**. Each subsystem receives only its own part, so changing upload sizes, for example, never touches device creation code:

```cpp
namespace avis::rhi
{
    // device creation: which GPU to use
    struct adapter_configuration
    {
        std::optional<adapter_luid> adapter;          // Visualizer passes Qt's adapter LUID
        bool allow_software_adapter = false;          // WARP (tests)
    };

    // diagnostics, applied before and right after device creation
    struct debug_configuration
    {
        bool debug_layer = false;                     // ID3D12Debug
        bool gpu_based_validation = false;            // ID3D12Debug1::SetEnableGPUBasedValidation (implies debug_layer)
        bool device_removed_extended_data = true;     // DRED auto-breadcrumbs + page-fault reporting
        bool break_on_error = false;                  // info-queue break (debugger attached)
    };

    // frame contexts (§4.2)
    struct frame_configuration
    {
        std::uint32_t frames_in_flight = 2;           // 1..3
    };

    // descriptor heaps (§6)
    struct descriptor_heap_configuration
    {
        std::uint32_t persistent_descriptor_count = 900'000;
        std::uint32_t dynamic_descriptor_count = 100'000;
        std::uint32_t sampler_count = 2'048;
        std::uint32_t render_target_view_count = 4'096;
        std::uint32_t depth_stencil_view_count = 1'024;
    };

    // CPU → GPU data paths (§8)
    struct upload_configuration
    {
        std::size_t staging_ring_size = 64ull << 20;
        std::size_t frame_allocator_size = 16ull << 20;   // per frame in flight
    };

    // aggregate, only used to pass everything to create(); each member goes to its own subsystem
    struct render_hardware_interface_configuration
    {
        adapter_configuration adapter;
        debug_configuration debug;
        frame_configuration frames;
        descriptor_heap_configuration descriptor_heaps;
        upload_configuration uploads;
    };

    class render_hardware_interface
    {
    public:
        static result<std::unique_ptr<render_hardware_interface>> create(const render_hardware_interface_configuration& configuration);
        const hardware_capabilities& capabilities() const noexcept;
        // resources, pipelines, queues, frames … (sections below)
    };
}
```

| Configuration | Consumed by (private implementation) | Section |
|---|---|---|
| `adapter_configuration` | adapter selection and device creation | §3.4 |
| `debug_configuration` | debug layer setup, info-queue routing, DRED | §12 |
| `frame_configuration` | frame context manager | §4.2 |
| `descriptor_heap_configuration` | descriptor heap allocators | §6 |
| `upload_configuration` | staging ring and frame allocators | §8 |

The Visualizer fills these from the `render` block of `visualizer.yaml` ([11 §14](11-renderer.md#14-configuration-visualizeryaml--render)).

### 3.3 Capability gate
`render_hardware_interface::create` fails with `error_domain::rhi` and a user-readable message if any check fails. The application then shows a dialog and **does not start**.

| Check | Query | Required |
|---|---|---|
| Feature level | `D3D12CreateDevice` | `D3D_FEATURE_LEVEL_12_0` |
| Shader model | `D3D12_FEATURE_SHADER_MODEL` | ≥ `D3D_SHADER_MODEL_6_6` |
| Resource binding tier | `D3D12_OPTIONS::ResourceBindingTier` | `TIER_3` |
| Resource heap tier | `D3D12_OPTIONS::ResourceHeapTier` | `TIER_2` (mixed buffers and textures in one heap, needed for aliasing) |
| Enhanced barriers | `D3D12_OPTIONS12::EnhancedBarriersSupported` | `TRUE` |
| Typed UAV loads | `D3D12_OPTIONS::TypedUAVLoadAdditionalFormats` | `TRUE` (R16G16B16A16_FLOAT / R11G11B10 UAV loads in TAA and lighting) |
| Interfaces | `QueryInterface` | `ID3D12Device10`, `ID3D12GraphicsCommandList7` |

Optional capabilities are recorded in `hardware_capabilities`: copy-queue timestamps (`D3D12_OPTIONS3::CopyQueueTimestampQueriesSupported`), wave size, and VRAM budget (`IDXGIAdapter3::QueryVideoMemoryInfo`).

### 3.4 Adapter selection
1. If `adapter_configuration::adapter` is set: `IDXGIFactory6::EnumAdapterByLuid`. This is mandatory for Qt interop, because shared handles must be opened on the same adapter.
2. Otherwise: `EnumAdapterByGpuPreference(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE)`, taking the first hardware adapter that passes the gate.
3. WARP only if `allow_software_adapter` is set.

### 3.5 Device lost
Every fence wait and submission checks for `DXGI_ERROR_DEVICE_REMOVED`. On detection: collect DRED breadcrumbs and page-fault data, log them, and throw `avis::rhi::device_lost` (unrecoverable, [01 §6](01-system-architecture.md#6-error-handling-policy)). Re-creating the device is out of scope for iteration 1.

## 4. Queues, synchronisation and frames

### 4.1 Queues
| `queue_type` | D3D12 | Use |
|---|---|---|
| `graphics` | DIRECT | Raster passes, everything that needs RTV/DSV |
| `compute` | COMPUTE | Async compute passes (light clustering, GTAO, histogram) |
| `copy` | COPY | Uploads and streaming, readback copies |

Each queue owns a **timeline fence**. `synchronization_point { queue_type queue; std::uint64_t value; }` names a point on that timeline.

```cpp
template<queue_type Queue>
class queue
{
public:
    synchronization_point submit(std::span<command_list<Queue>* const> lists,
                                 std::span<const synchronization_point> waits = {});
    void signal(const shared_fence& fence, std::uint64_t value);      // §11
    void wait(const shared_fence& fence, std::uint64_t value);        // §11
};

bool render_hardware_interface::is_complete(synchronization_point point) const;
auto render_hardware_interface::when_complete(synchronization_point point) -> sender_of<void>;              // §4.2
auto render_hardware_interface::when_complete(std::span<const synchronization_point> points) -> sender_of<void>;
void render_hardware_interface::wait_idle();                       // blocking; shutdown and tests only
```

- `queue<Queue>::submit` only accepts command lists recorded for the same queue type, which is checked at compile time (§4.3).
- Cross-queue waits use `ID3D12CommandQueue::Wait` on the producer queue's fence, issued before `ExecuteCommandLists`. These are **GPU-side** waits and never block a CPU thread.
- `wait_idle` is the only CPU-blocking call in the RHI. It's used at shutdown and in tests. Resizing the shared output ring doesn't need it ([12 §3.3](12-visualizer.md#33-shared-frame-ring)).

### 4.2 Frame contexts (sender-based, non-blocking)

A frame context bundles the per-frame resources that can only be reused once the GPU has finished the frame that last used them:
- command allocator pools (per queue type × per recording thread)
- the dynamic descriptor region slice (§6.2)
- the frame linear allocator (upload heap) for constants and transient CPU-written data (§8.2)
- the deferred-release list resolved against this frame's synchronization points

There are `frame_configuration::frames_in_flight` contexts (*F*), used round-robin: frame *N* uses context *N mod F*.

**Acquiring a frame never blocks a thread.** `begin_frame()` returns a **sender** that completes once the context is free:

```cpp
// move-only; identifies frame N and its context N mod F
class frame_token;

// completes with set_value(frame_token) once the GPU has finished every submission of frame N - F,
// set_error(avis::error) on device removal, set_stopped() if the receiver's stop token fires first
auto render_hardware_interface::begin_frame() -> sender_of<frame_token>;

// records the last submission of each queue for this frame; the context becomes reusable once all are complete
void render_hardware_interface::end_frame(frame_token&& frame, std::span<const synchronization_point> last_submissions);

// non-blocking poll for tools and tests: a token if the next context is already free, otherwise std::nullopt
std::optional<frame_token> render_hardware_interface::try_begin_frame();
```

**How completion works:**
1. `begin_frame()`'s operation state checks the fence values recorded by `end_frame` for context *N mod F*. If they are all complete, it completes **inline** with the token (the common case when the GPU keeps up).
2. Otherwise it arms one Win32 event with `ID3D12Device1::SetEventOnMultipleFenceCompletion` (all of the context's queue fences, `D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL`). The event is waited on asynchronously with `asio::windows::object_handle::async_wait` on the default IO context ([07](07-io-layer.md)). When it fires, the IO thread completes the receiver. Continuations move to the task system with `continues_on`. **No thread sleeps on the fence.** The same mechanism implements `when_complete` and readback (§10).
3. Stop: a `stop_callback` cancels the `async_wait`, and the sender completes with `set_stopped()`.
4. The context's command allocators are reset and its dynamic descriptor slice and frame allocator are rewound **in the completion**, before the token is delivered. Deferred releases tagged with that frame's points are freed at the same time.

**Ordering rules:**
- Frames are strictly ordered. Only **one** `begin_frame()` may be outstanding, and the next one may only be started after `end_frame` for the current token. Violations are asserted in debug builds.
- The `frame_token` must be passed to `end_frame` exactly once. Destroying an unfinished token is a programming error (asserted), because the context would never become reusable.

**Back-pressure without blocking:** if the GPU falls *F* frames behind, the next `begin_frame()` simply doesn't complete yet. The frame chain that waits on it isn't running, so the CPU does other work (simulation, decoding) in the meantime. Memory stays bounded at *F* contexts, and latency stays bounded at *F* frames.

**The frame loop as a sender chain** (owned by the Visualizer's renderer, [12 §3.4](12-visualizer.md#34-frame-pacing)):

```cpp
auto frame_once(renderer& renderer)
{
    auto& hardware_interface = renderer.hardware_interface();
    auto tasks = avis::runtime::task_system().get_scheduler();
    return stdexec::when_all(hardware_interface.begin_frame(), renderer.next_frame_tick())  // context free + pacing timer
         | stdexec::continues_on(tasks)
         | stdexec::then([&](frame_token frame) { return renderer.prepare(std::move(frame)); })   // extract, build, compile graph
         | stdexec::let_value([&](prepared_frame& frame) { return renderer.record(frame); })      // bulk over recording groups
         | stdexec::then([&](prepared_frame frame) { renderer.submit_and_end(std::move(frame)); });  // submit, end_frame
}
// the renderer re-arms frame_once() from its completion inside an async scope until stop is requested
```

### 4.3 Command lists and multi-threaded recording

A command list is a **typed view**, one template instance per queue type. D3D12 queue capabilities nest (a direct list can do everything, a compute list can do compute and copy, a copy list can only copy), so each operation is constrained with a `requires` clause to the queue types that support it:

```cpp
namespace avis::rhi
{
    // Non-owning, move-only view of a command list recorded for queue type Queue.
    // The owning record (com_ptr<ID3D12GraphicsCommandList7>, its allocator, bound pipeline state)
    // lives in a private pool inside render_hardware_interface.
    template<queue_type Queue>
    class command_list
    {
    public:
        // ---- every queue type: copy, synchronization, debug ----
        void copy_buffer(buffer_handle destination, std::uint64_t destination_offset,
                         buffer_handle source, std::uint64_t source_offset, std::uint64_t size);
        void copy_texture(texture_handle destination, subresource destination_subresource,
                          texture_handle source, subresource source_subresource);
        void copy_buffer_to_texture(texture_handle destination, subresource destination_subresource,
                                    buffer_handle source, std::uint64_t source_offset);
        void copy_texture_to_buffer(buffer_handle destination, std::uint64_t destination_offset,
                                    texture_handle source, subresource source_subresource);
        void barrier(std::span<const global_barrier> global_barriers,
                     std::span<const buffer_barrier> buffer_barriers,
                     std::span<const texture_barrier> texture_barriers);
        void begin_event(std::string_view name);
        void end_event();
        gpu_scope_id begin_timing(std::string_view name);
        void end_timing(gpu_scope_id scope);

        // ---- compute and graphics queues ----
        void set_pipeline(compute_pipeline_handle pipeline)
            requires (Queue != queue_type::copy);
        void push_constants(std::span<const std::uint32_t> values, std::uint32_t offset = 0)      // root parameter 0
            requires (Queue != queue_type::copy);
        void set_pass_constants(gpu_address address)                                              // root parameter 1
            requires (Queue != queue_type::copy);
        void set_view_constants(gpu_address address)                                              // root parameter 2
            requires (Queue != queue_type::copy);
        void dispatch(std::uint32_t group_count_x, std::uint32_t group_count_y, std::uint32_t group_count_z)
            requires (Queue != queue_type::copy);
        void dispatch_indirect(buffer_handle arguments, std::uint64_t offset)
            requires (Queue != queue_type::copy);
        void clear_unordered_access_float(texture_view view, std::span<const float, 4> values)
            requires (Queue != queue_type::copy);

        // ---- graphics queue only ----
        void set_pipeline(graphics_pipeline_handle pipeline)
            requires (Queue == queue_type::graphics);
        void set_render_targets(std::span<const texture_view> colors, std::optional<texture_view> depth)
            requires (Queue == queue_type::graphics);
        void set_viewport(const viewport& value)
            requires (Queue == queue_type::graphics);
        void set_scissor(const rectangle& value)
            requires (Queue == queue_type::graphics);
        void set_index_buffer(buffer_handle buffer, index_type type, std::uint64_t offset = 0)
            requires (Queue == queue_type::graphics);
        void draw(std::uint32_t vertex_count, std::uint32_t instance_count = 1,
                  std::uint32_t first_vertex = 0, std::uint32_t first_instance = 0)
            requires (Queue == queue_type::graphics);
        void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count = 1, std::uint32_t first_index = 0,
                          std::int32_t vertex_offset = 0, std::uint32_t first_instance = 0)
            requires (Queue == queue_type::graphics);
        void draw_indirect(buffer_handle arguments, std::uint64_t offset, std::uint32_t maximum_count,
                           std::optional<buffer_handle> count = {})
            requires (Queue == queue_type::graphics);
        void clear_render_target(texture_view view, std::span<const float, 4> color)
            requires (Queue == queue_type::graphics);
        void clear_depth(texture_view view, float depth)
            requires (Queue == queue_type::graphics);
    };

    using graphics_command_list = command_list<queue_type::graphics>;
    using compute_command_list = command_list<queue_type::compute>;
    using copy_command_list = command_list<queue_type::copy>;

    template<queue_type Queue>
    command_list<Queue> render_hardware_interface::acquire_command_list(const frame_token& frame);   // thread-safe
}
```

**Design rules:**
- **Views, not owners.** `command_list<Queue>` is a small move-only handle (a pointer to a private record plus a generation). The native list, allocator and tracked state stay in the RHI's private pool, held through `com_ptr` (§3.1). No D3D12 type is visible.
- **Compile-time queue safety.** Calling `draw()` on a compute list doesn't compile, and neither does submitting a graphics list to the compute queue: `queue<queue_type::compute>::submit` only accepts `command_list<queue_type::compute>`. Distinct template instances, rather than an inheritance hierarchy, keep a graphics list from passing as a compute list.
- **Pipeline-aware root binding.** On a graphics list, `push_constants`, `set_pass_constants` and `set_view_constants` go to the graphics or compute root slots depending on the kind of the last `set_pipeline` call. Compute work on the graphics queue uses the same API. Both pipeline kinds use the global root signature (§6.3).
- **Typed pipeline handles.** `graphics_pipeline_handle` and `compute_pipeline_handle` are distinct types (§9.2), so overload resolution picks the right `set_pipeline`.
- **Lifetime.** `acquire_command_list` opens a list from the frame context's allocator pool. `queue<Queue>::submit` closes it. A list must be submitted within the frame it was acquired for, and lists and allocators are recycled when the frame context is reused (§4.2).
- **Multi-threaded recording.** Allocators are pooled **per thread per frame per queue type**. The pool is picked by the dense per-thread index `avis::threading::thread_pool::current_index()` ([06 §3.4](06-thread-pool.md#34-options-and-hooks)), so acquiring a list is lock-free on the hot path. Every thread that records (the task-system workers) is a pool thread. Threads outside the pool (Qt threads, the main thread) use a shared, mutex-protected fallback pool. A list is used by one thread at a time.
- **Submission order** across lists is the caller's responsibility. The render graph handles it ([10 §7](10-render-graph.md#7-execution-phase)).
- **Recording-only narrowed views.** A graphics list can hand out a view restricted to the compute or copy subset:
  ```cpp
  template<queue_type Narrower>
      requires (Narrower == queue_type::compute || Narrower == queue_type::copy)
  command_list<Narrower> command_list<queue_type::graphics>::narrow() const;   // shares the same native list
  ```
  The result exposes only the narrower operation set, but records into the underlying direct list. It's flagged *recording-only*: passing it to any `queue<…>::submit` is rejected (asserted in debug builds, `error_domain::rhi` in release), so compute or copy work recorded on the graphics queue can't be submitted to the wrong queue. The render graph is the main user: it narrows when a compute or copy pass falls back to the graphics queue.
- **Render graph integration.** The render graph fixes the view type by the add function used for a pass ([10 §3.1](10-render-graph.md#31-building)):

| Add function | Assigned queue | View in the pass context |
|---|---|---|
| `add_graphics_pass` | graphics | `graphics_command_list` |
| `add_compute_pass` | compute (async) | `compute_command_list` |
| `add_compute_pass` | graphics (async compute disabled or not hinted) | `compute_command_list`, narrowed from the graphics list |
| `add_copy_pass` | copy | `copy_command_list` |
| `add_copy_pass` | graphics (fallback, [10 §6.2](10-render-graph.md#62-scheduling-and-queue-assignment)) | `copy_command_list`, narrowed from the graphics list |

## 5. Resources and memory

### 5.1 Descriptions
```cpp
enum class texture_usage : std::uint32_t { shader_resource = 1, unordered_access = 2, render_target = 4, depth_stencil = 8, shared = 16, copy_source = 32, copy_destination = 64 };
enum class memory_type   : std::uint8_t  { gpu_only, upload, readback };

struct texture_description
{
    texture_dimension dimension = texture_dimension::texture_2d;     // texture_1d, texture_2d, texture_3d, cube
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depth_or_layers = 1;
    std::uint32_t mip_levels = 1;
    rhi::format format = rhi::format::rgba8_unorm;
    enum_flags<texture_usage> usage = texture_usage::shader_resource;
    std::optional<clear_value> optimized_clear;
    std::string_view debug_name;
};

struct buffer_description
{
    std::uint64_t size = 0;
    std::uint32_t structure_stride = 0;        // 0 = raw/byte-address
    enum_flags<buffer_usage> usage;            // shader_resource, unordered_access, index, indirect_arguments, constant
    memory_type memory = memory_type::gpu_only;
    std::string_view debug_name;
};
```

`format` is an Avis enum covering the formats in use (RGBA8 UNORM/SRGB, RGB10A2, R11G11B10F, RGBA16F, RG16F, RG16 UNORM/SNORM, R8, R16F, R32F, RG32F, RGBA32F, D32F, BC1/3/4/5/7 variants). A private table maps them to DXGI.

### 5.2 Handles and lifetime
- `texture_handle` / `buffer_handle` / `graphics_pipeline_handle` / `compute_pipeline_handle` / `sampler_handle` are 32-bit index + 32-bit generation into slot maps. Stale handles are detected.
- `destroy(handle)` is **deferred**. The resource goes onto a release queue tagged with the latest *submitted* value of every queue. It's freed, and its bindless indices are returned to the free list, only once all three tagged values have completed. Callers can destroy resources right after their last recording.
- Allocation uses **D3D12MemoryAllocator** (`CreateResource3` with an initial `D3D12_BARRIER_LAYOUT`), committed or sub-allocated based on D3D12MA heuristics.
- **Placed resources for aliasing:** `create_heap(size, heap_kind)` and `create_placed_texture(heap, offset, description)` / `create_placed_buffer(...)`, via `D3D12MA::Allocator::CreateAliasingResource2`. They're used only by the render graph's transient pool ([10 §6.4](10-render-graph.md#64-aliasing-and-transient-memory)).
- **Residency:** no explicit `MakeResident`/`Evict` in iteration 1. The budget is tracked and warnings are logged above 90% of local VRAM.

### 5.3 Views
Each texture gets these descriptors at creation, based on usage:
- an SRV over all mips/layers (bindless index `srv`)
- UAVs per mip (bindless indices `uav[mip]`)
- RTV/DSV per mip/layer in CPU-only heaps, created lazily and cached

`texture_view { texture_handle t; std::uint16_t mip; std::uint16_t first_layer; std::uint16_t layer_count; format view_format; }` selects RTV/DSV/UAV subsets.

## 6. Bindless descriptor model

### 6.1 Heaps
| Heap | Visibility | Size (default) | Allocator |
|---|---|---|---|
| CBV/SRV/UAV | **shader-visible**, one per device, lifetime of the application | 1,000,000 | Regions (§6.2) |
| CBV/SRV/UAV staging | CPU-only, mirrors the indices of the persistent region | 900,000 | Same indices as persistent |
| Sampler | **shader-visible** | 2,048 | Free list |
| RTV | CPU-only | 4,096 | Free list |
| DSV | CPU-only | 1,024 | Free list |

The two shader-visible heaps are bound once per command list (`SetDescriptorHeaps`) on acquisition.

### 6.2 Regions of the shader-visible CBV/SRV/UAV heap
```
index 0 ─────────────────────────────── 899,999 │ 900,000 ───────────── 999,999
      persistent / static region (lower range)  │ dynamic region (upper range)
      free list with deferred recycling         │ ring, split into frames_in_flight slices
```
- **Persistent (lower range):** long-lived SRVs and UAVs for textures, material buffers, mesh buffers and global buffers. Allocation is a lock-free free list (a tagged-index stack). Freed indices are recycled only after the GPU has finished with them (§5.2). Descriptors are written to the **staging heap** at the same index first, then copied to the shader-visible heap with `CopyDescriptorsSimple` at creation or streaming time, as the idea document specifies. The CPU copy is kept because `ClearUnorderedAccessView*` needs a CPU-visible handle.
- **Dynamic (upper range):** per-frame descriptors for render-graph transient resources and temporary views. Each frame in flight owns a slice. Allocation is an atomic bump pointer inside the slice, which is reset in `begin_frame` once the frame's previous use has completed. That makes the region a lock-free ring.
- Index `0` of each region holds a **null descriptor**, so an invalid index samples zero instead of faulting under GBV.

### 6.3 Global root signature
One root signature (version 1.1) is shared by **every** graphics and compute PSO:

| Root param | Type | Register | DWORDs | Use |
|---|---|---|---|---|
| 0 | 32-bit constants ×16 | `b0, space0` | 16 | Per-draw/dispatch constants: bindless indices, material id, instance offset, packed 64-bit buffer addresses |
| 1 | Root CBV | `b1, space0` | 2 | Per-pass constants (GPU VA from the frame allocator) |
| 2 | Root CBV | `b2, space0` | 2 | Per-view/frame constants (camera, jitter, time, exposure) |

Total: 20 of 64 DWORDs.

Flags: `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED`. There's no input assembler, because vertices are pulled from buffers.

**Static samplers** (free, no heap slot) for the common set: `s0` point-clamp, `s1` linear-clamp, `s2` linear-wrap, `s3` anisotropic×16-wrap, `s4` shadow comparison (`GREATER_EQUAL`, reverse-Z, linear), `s5` point-wrap. Any other sampler is created in the sampler heap and indexed through `SamplerDescriptorHeap[i]`.

### 6.4 Shader-side conventions
The RHI publishes the global root signature layout (register numbers, root parameter sizes, static sampler slots) as C++ constants in `include/avis/rhi/root_signature_layout.h`. The library ships **no HLSL**: shaders are application data. The Visualizer's `data/shaders/visualizer/include/bindless.hlsli` mirrors those constants, and a Visualizer test compiles a probe shader to check they agree ([11 §13](11-renderer.md#13-shaders-location-and-build)). The header looks like this:

```hlsl
struct draw_constants           // root param 0: exactly 16 DWORDs = four 16-byte constant registers
{
    uint instance_buffer;       // register 0: bindless SRV index (StructuredBuffer<instance_data>)
    uint material_buffer;
    uint vertex_buffer;         // ByteAddressBuffer
    uint index_offset;
    uint first_instance;        // register 1
    uint3 user;                 // pass-specific
    uint4 extra[2];             // registers 2-3: pass-specific, or packed 64-bit buffer addresses
};
ConstantBuffer<draw_constants> g_draw : register(b0);

template<typename T> T load_structured(uint heap_index, uint element)
{
    StructuredBuffer<T> buffer = ResourceDescriptorHeap[NonUniformResourceIndex(heap_index)];
    return buffer[element];
}
Texture2D<float4> texture2d(uint heap_index) { return ResourceDescriptorHeap[NonUniformResourceIndex(heap_index)]; }
```

- **Packing rule for root-constant structs:** HLSL constant-buffer packing starts every array element on a new 16-byte register, and a vector can't straddle a register boundary. Arrays in `draw_constants` (and any other root-constant struct) must therefore use `uint4` / `float4` elements only, otherwise the struct silently grows beyond the 16 DWORDs of root parameter 0. The C++ mirror in `shared_types.hlsli` has `static_assert(sizeof(draw_constants) == 64)` and `offsetof` checks, and the probe-shader test ([11 §13](11-renderer.md#13-shaders-location-and-build)) compares the reflected HLSL size against `rhi::root_signature_layout::draw_constant_count`.
- Indices go into root constants. Per-pass and per-view data go into root CBVs. 64-bit GPU virtual addresses can be packed into two root constants for buffer-pointer-style access.
- `NonUniformResourceIndex` is required whenever an index can vary within a wave (materials per pixel).
- C++/HLSL shared structs live in `*.hlsli` headers that also compile as C++ through `#ifdef __cplusplus` type aliases (`uint` → `std::uint32_t`, `float4x4` → `Eigen::Matrix4f` with matching column-major packing).

## 7. Barriers (enhanced barriers)

### 7.1 Abstraction
```cpp
struct access_state
{
    enum_flags<pipeline_synchronization> synchronization;     // ↔ D3D12_BARRIER_SYNC   (vertex_shading, pixel_shading, compute_shading, render_target, depth_stencil, copy, all, none…)
    enum_flags<resource_access> access; // ↔ D3D12_BARRIER_ACCESS (shader_resource, unordered_access, render_target, depth_read, depth_write, copy_source, copy_destination, indirect, no_access…)
    texture_layout layout;              // ↔ D3D12_BARRIER_LAYOUT (undefined, common, shader_resource, unordered_access, render_target, depth_read, depth_write, copy_source, copy_destination, direct_queue_*, compute_queue_*)
};

struct texture_barrier
{
    texture_handle texture;
    subresource_range range;
    access_state before;
    access_state after;
    bool discard = false;
};

struct buffer_barrier
{
    buffer_handle buffer;
    access_state before;
    access_state after;
};

struct global_barrier
{
    access_state before;
    access_state after;
};
```
These map 1:1 to `D3D12_TEXTURE_BARRIER` / `D3D12_BUFFER_BARRIER` / `D3D12_GLOBAL_BARRIER` and are grouped into one `Barrier()` call per batch.

### 7.2 Rules
- The RHI **doesn't track state automatically**. The render graph computes barriers ([10 §6.3](10-render-graph.md#63-barriers)). Code outside the graph (uploads, readback, interop) uses helpers with explicit states.
- **Copy queue:** textures accessed on the copy queue must use `texture_layout::common`.
- **Cross-queue:** textures shared between graphics and async compute within a frame use the common-compatible layouts (`common`, `shader_resource`, `unordered_access`, which work on both queues) or queue-specific layouts with an explicit transition on the owning queue.
- **Aliasing:** the first use of an aliased placed resource gets `before.layout = undefined`, `synchronization = none`, `access = no_access`, and `discard = true` for render/depth targets.
- **Cross-device shared textures (Qt interop):** these must be in `texture_layout::common` at hand-off ([12 §3](12-visualizer.md#3-viewport-integration-qquickrhiitem--shared-handles)).

## 8. Uploads

### 8.1 Staging ring buffer
- One persistently mapped `UPLOAD` buffer (default 64 MiB) used as a ring. Each allocation is tagged with the **copy-queue** synchronization point of the submission that consumes it. The tail advances past entries whose synchronization points have completed.
- **Allocation never blocks a thread.** When a request doesn't fit, the synchronous call fails with `rhi::errc::upload_ring_full` instead of waiting. The asynchronous variant returns a sender that completes once enough of the ring has been retired: it waits on the oldest pending synchronization points with `when_complete` (§4.2, §10), so the wait happens on an IO completion, not on a sleeping thread. `wait_idle` stays the only blocking RHI call.
- Requests larger than half the ring get a dedicated temporary upload buffer, freed through deferred destruction. They never wait for ring space.
- API:
```cpp
// non-blocking: fails with errc::upload_ring_full when the ring has no room right now
result<upload_ticket> upload_buffer(buffer_handle destination, std::uint64_t destination_offset, std::span<const std::byte> data);
result<upload_ticket> upload_texture(texture_handle destination, std::span<const subresource_data> data);   // footprints computed internally

// back-pressure without blocking: completes (on the IO context) once the data has been placed in the ring;
// `data` must stay valid until completion
auto upload_buffer_async(buffer_handle destination, std::uint64_t destination_offset, std::span<const std::byte> data)
    -> sender_of<upload_ticket>;
auto upload_texture_async(texture_handle destination, std::span<const subresource_data> data)
    -> sender_of<upload_ticket>;

synchronization_point flush_uploads();      // records + submits pending copies on the copy queue
```
- Bulk streaming (asset loading) uses the `_async` forms. Code in the frame chain uses the synchronous form for small per-frame data, and treats `upload_ring_full` as "defer to the next frame".
- An `upload_ticket` wraps the copy-queue `synchronization_point`. Consumers on other queues wait on it. The render graph does this automatically for imported resources with pending tickets.
- Textures arrive in `common` layout after copy-queue uploads, and their first graphics or compute use transitions them.

### 8.2 Frame linear allocator
For data written every frame (constants, instance data, light lists): a per-frame-context upload-heap linear allocator. `allocate(size, alignment = 256) -> { void* cpu; gpu_address gpu; }`. It resets in `begin_frame`. Root CBVs point straight into it.

## 9. Pipelines and shaders

### 9.1 Shader library
- The application's build compiles its `.hlsl` with DXC into a shader directory of its choice (the Visualizer uses `<bin>/data/shaders/visualizer/`), as `<name>.<entry>.<stage>.cso` plus `manifest.yaml` listing ids, stages, entry points, defines and xxh3 hashes ([13 §3](13-build-and-quality.md#3-shader-build-pipeline)). YAML is used because yaml-cpp is already a dependency (scenarios, configuration), so no JSON library is needed:
  ```yaml
  avis_shader_manifest: 1
  shaders:
    - id: lighting.main.cs
      file: lighting.main.cs.cso
      stage: cs
      entry: main
      defines: []
      xxh3: "5f0c1e2a9b3d7a41"
  ```
- `shader_library::load(directory) -> sender_of<shader_library>` takes the directory from the application (the RHI has no built-in shader paths), reads the manifest (parsed with yaml-cpp) and every blob through the IO layer.
- `shader_id` is a compile-time hashed string (`"lighting.main.cs"_shader_id`), and the manifest maps ids to blobs. A missing shader is a startup error.
- No hot reload, as the idea document specifies.

### 9.2 PSO cache
```cpp
struct graphics_pipeline_description
{
    shader_id vertex_shader;
    shader_id pixel_shader;
    rasterizer_state rasterizer;    // cull, fill, depth bias, conservative rasterization off
    depth_state depth;              // test, write, compare (default GREATER_EQUAL, reverse-Z)
    std::array<blend_state, 8> blend;
    std::uint32_t render_target_count;
    std::array<format, 8> render_target_formats;
    format depth_format;
    primitive_topology topology = primitive_topology::triangle_list;
    std::string_view debug_name;
};
struct compute_pipeline_description
{
    shader_id compute_shader;
    std::string_view debug_name;
};

// thread-safe; distinct handle types select the matching command_list::set_pipeline overload (§4.3)
result<graphics_pipeline_handle> render_hardware_interface::create_pipeline(const graphics_pipeline_description& description);
result<compute_pipeline_handle>  render_hardware_interface::create_pipeline(const compute_pipeline_description& description);
```
- PSOs are built with pipeline state streams (`ID3D12Device2::CreatePipelineState`), always using the global root signature.
- The cache is keyed by a hash of the description (shader blob hashes included). Lookup is lock-free for reads; inserts take a mutex.
- The renderer creates all known PSOs at startup, in parallel, with a `stdexec::bulk` over the pipeline descriptions on the task system ([15](15-task-system.md)). No pipeline libraries or on-disk PSO caching.

## 10. Readback
```cpp
auto render_hardware_interface::readback_buffer(buffer_handle, std::uint64_t offset, std::uint64_t size, queue_type queue = queue_type::copy)
    -> sender_of<readback_data>;
auto render_hardware_interface::readback_texture(texture_handle, subresource, queue_type queue = queue_type::graphics)
    -> sender_of<readback_image>;           // includes row pitch, format
```
- A copy into a pooled `READBACK` buffer is recorded and submitted, and the request completes when the synchronization point does.
- **Fence completion as a sender:** `when_complete(synchronization_point)` uses `ID3D12Fence::SetEventOnCompletion` (or `SetEventOnMultipleFenceCompletion` for several points) with an event wrapped in `asio::windows::object_handle`, the same mechanism as `begin_frame` (§4.2). Its `async_wait` runs on the IO context ([07](07-io-layer.md)), so GPU completions join the same sender machinery with no polling thread.
- Uses: the debug pixel inspector, screenshots, GPU timing results, tests (golden images).

## 11. Cross-device sharing (Qt interop)
```cpp
struct shared_handle { void* value; };            // opaque OS handle (NT HANDLE); no D3D type
result<texture_handle> create_shared_texture(const texture_description& description);   // usage must include texture_usage::shared
result<shared_handle>  export_shared_handle(texture_handle);
result<shared_fence>   create_shared_fence();       // D3D12_FENCE_FLAG_SHARED
result<shared_fence>   import_shared_fence(shared_handle);
result<shared_handle>  export_shared_handle(const shared_fence&);
void queue::signal(const shared_fence&, std::uint64_t value);
void queue::wait(const shared_fence&, std::uint64_t value);

// CPU-side observation of a shared fence, whichever device signals it (never blocks)
std::uint64_t shared_fence::completed_value() const noexcept;            // ID3D12Fence::GetCompletedValue
auto render_hardware_interface::when_reached(const shared_fence& fence, std::uint64_t value) -> sender_of<void>;
```
- `when_reached` uses the same mechanism as `when_complete` (§10): `SetEventOnCompletion` with an event waited on through `asio::windows::object_handle` on the IO context. It completes inline when the value has already been reached, and with `set_stopped()` if its stop token fires first. It works for fences signalled by another device too, which is how the Visualizer observes Qt's `consumed` fence without ever making an Avis queue wait on Qt ([12 §3.3](12-visualizer.md#33-shared-frame-ring)).
- `queue::wait` on a shared fence is a GPU-side wait that can't be cancelled. Avis code must only use it on fences whose signal is already guaranteed to be submitted. The Visualizer doesn't use it for Qt interop.
- Shared textures are **committed** with `D3D12_HEAP_FLAG_SHARED`. They're never placed or aliased.
- Handles are created with `CreateSharedHandle(..., GENERIC_ALL)`. The exporting side owns them and closes them with `CloseHandle` on destruction.
- See [12 §3](12-visualizer.md#3-viewport-integration-qquickrhiitem--shared-handles) for the full protocol.

## 12. Debugging and profiling
| Feature | Mechanism |
|---|---|
| Debug layer | `D3D12GetDebugInterface(ID3D12Debug)->EnableDebugLayer()` before device creation |
| GPU-based validation | `ID3D12Debug1::SetEnableGPUBasedValidation(TRUE)` (slow; off by default) |
| Message routing | `ID3D12InfoQueue1::RegisterMessageCallback` → spdlog `rhi` logger. Break on corruption/error is optional. |
| DRED | `ID3D12DeviceRemovedExtendedDataSettings1`: auto-breadcrumbs, page-fault reporting, breadcrumb context |
| Object names | `SetName` with `debug_name` on every resource, PSO, queue and list |
| PIX events | `begin_event`/`end_event` emit `BeginEvent`/`EndEvent` with PIX-compatible Unicode metadata. Every render-graph pass is wrapped automatically. |
| GPU timings | Timestamp query heaps per queue per frame, resolved into readback buffers, results available `frames_in_flight` frames later, shown in the Visualizer statistics overlay |

Debug flags come from `visualizer.yaml` (`render.debug_layer`, `render.gpu_validation`) and from `--debug-layer` / `--gpu-validation`.

## 13. Threading summary
| Operation | Thread safety |
|---|---|
| Resource, pipeline and descriptor creation/destruction | Thread-safe (slot maps with fine-grained locks, lock-free descriptor allocators) |
| `acquire_command_list` | Thread-safe, lock-free per thread |
| Recording into a `command_list<Queue>` | One thread at a time |
| `begin_frame`, `end_frame`, `queue<Queue>::submit` | Never block. Called from any thread, but **serialised by the frame chain** (one frame stage at a time, asserted in debug builds) |
| `when_complete`, `begin_frame` completion, readback | Completion on an IO thread, continued on the task system |
| `wait_idle` | The only blocking call: shutdown and tests |
| Uploads (`upload_*`, `upload_*_async`) | Thread-safe (ring allocation is atomic) and never block. A full ring fails the synchronous form or delays the sender's completion. `flush_uploads` is called from the frame chain. |
