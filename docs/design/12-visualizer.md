# 12 — Visualizer Application

## Code placement

| Feature | Namespace | CMake target | Location |
|---|---|---|---|
| Visualizer application (controller, simulation, UI models) | `avis::visualizer` | `AVIS_VISUALIZER` | `apps/visualizer/` |
| Scene representation (semantics → meshes + materials, §9) | `avis::visualizer::representation` | `AVIS_VISUALIZER` | `apps/visualizer/representation/` |
| Scene renderer (scene-agnostic, [11](11-renderer.md)) | `avis::visualizer::rendering` | `AVIS_VISUALIZER_RENDERING`, a static library without Qt, linked by `AVIS_VISUALIZER` and `AVIS_TESTS` | `apps/visualizer/rendering/` |
| Scene renderer shaders (HLSL data) | — | `AVIS_VISUALIZER_RENDERING` (`avis_add_shaders`) | `data/shaders/visualizer/scene/`, `include/` → `<bin>/data/shaders/visualizer/` |
| Shader manifest parser (`manifest.yaml` → `graphics::shader_manifest`, §10) | `avis::visualizer::rendering` | `AVIS_VISUALIZER_RENDERING` | `apps/visualizer/rendering/shader_manifest_parser.{h,cpp}` |
| QML UI | QML module `Avis.Visualizer` | `AVIS_VISUALIZER` (`qt_add_qml_module`) | `apps/visualizer/qml/` |
| Qt-side blit shaders | — (GLSL → `.qsb`) | `AVIS_VISUALIZER` (`qt_add_shaders`) | `data/shaders/visualizer/qt/` |

## 1. Purpose and scope
The Visualizer is the application a user works in to **open, edit and save a simulation run, connect to a Driver, control the simulation and view the results** ([01 §2.1](01-system-architecture.md#21-visualizer-purpose-and-tasks)):
- open, edit and save scenarios: select the data source and input sensors from the Driver's catalogue, the plug-ins and their parameters (editing only while stopped)
- discover Drivers with Zenoh scouting and connect to one
- own the virtual clock: start, pause, single-step, stop and set the speed of the simulation
- keep the active scene, which starts empty and is filled by the Driver's scene stream ([02 §5.8](02-communication.md#58-scene-stream))
- render the scene in real time (Avis renderer, [11](11-renderer.md)) in a Qt Quick UI
- inspect the scene: a tree view of all entities, an entity inspector with synchronised selection, and a free-fly and a follow camera
- debug rendering: resource blit selection, GPU timings

It is **agnostic to the vehicle model**. It doesn't read datasets, interpret sensor data or know where a vehicle can drive: it shows what the Driver sends.

UI technology: **Qt 6.11.1, Qt Quick / QML**. The 3D viewport uses **`QQuickRhiItem`**. Qt and the Avis RHI each create their D3D12 device, with their own queues, sharing frames through **shared handles and fences** (idea.md). In one process both get the same device object (§3.1).

## 2. Source layout
```
apps/visualizer/
  main.cpp                         # arguments, configuration, avis::runtime, QGuiApplication, D3D12 backend selection
  visualizer_driver.{h,cpp}        # application controller: owns simulation thread, frame chain, network session
  simulation/
    simulation_thread.{h,cpp}      # tick loop (03), apply_result, snapshot publish
    simulation_command.h, command_queue.h, gui_sink.h   # GUI ↔ simulation (§4)
    demo_scene.{h,cpp}             # --demo-scene: a local scene source for Part A, tests and offline UI work
    driver_discovery.{h,cpp}       # scouting + describe (02 §2)
    lockstep_coordinator.{h,cpp}   # connect, load, step, unload (02)
    scene_stream.{h,cpp}           # stage scene stream messages per step, commit, apply (02 §6.3, 08 §3.1)
    scenario_model.{h,cpp}         # in-memory scenario + YAML load/save + validation (04)
  representation/                  # streamed content → meshes + materials (§9); runs on the simulation thread
    scene_representation.h         # the step-4 interface the simulation thread calls
    presentation_state.{h,cpp}     # what the presentation commands set (§4.1)
    representation_system.{h,cpp}
    shape_representation.{h,cpp}   # box, vehicle, axes, frustum display shapes
    geometry_representation.{h,cpp}  # belief geometry chunks
    lidar_representation.{h,cpp}   # lidar sweeps → point meshes
    label_representation.{h,cpp}   # labels → glyph-quad meshes on the font atlas (08 §8.1)
    trail_representation.{h,cpp}   # trails → chunked world-space line meshes (08 §8.2)
    stale_representation.{h,cpp}   # dimmed material variants for stale entities (08 §8.4)
    helper_geometry.{h,cpp}        # grid, axes gizmo
  rendering/                       # scene-agnostic renderer (11)
    frame_loop.{h,cpp}             # sender-based frame chain (17 §4), RHI, render graph
    screenshot_capture.{h,cpp}     # capture pass, custom-resolution view, PNG encode (11 §12.1)
    scene_renderer.{h,cpp}         # pass setup
    draw_lists.{h,cpp}             # culling, (key, value) items, 64-bit sort keys, sorting, run building (11 §4)
    pipeline_variant_registry.{h,cpp}
    shader_manifest_parser.{h,cpp}   # manifest.yaml → graphics::shader_manifest (§10)
    render_system.{h,cpp}          # the RHI and every graphics object, created and destroyed in order (§3.2, §3.4)
    shared_frame_ring.{h,cpp}      # interop ring (§3.3): textures, fences, the slot gate, resize
    ring_protocol.{h,cpp}          # the ring's CPU protocol, testable without a GPU (§3.3)
  ui/
    viewport_item.{h,cpp}          # QQuickRhiItem subclass (replaces viewport.{h,cpp})
    viewport_renderer.{h,cpp}      # QQuickRhiItemRenderer: imports shared textures, blits
    simulation_bridge.{h,cpp}      # gui_sink on the GUI thread: queued calls, latest values, signals
    renderer_bridge.{h,cpp}        # frame_sink on the GUI thread: debug information, picks, inspections, screenshots
    camera_controller.{h,cpp}
    scene_tree_model.{h,cpp}       # QAbstractItemModel
    inspector_model.{h,cpp}        # reflected component values + Driver properties
    render_debug_model.{h,cpp}     # render-graph resources/passes, selection
    log_model.{h,cpp}              # local + driver logs
    commands/                      # QUndoCommand implementations
  qml/
    main.qml, Viewport.qml, SceneTree.qml, Inspector.qml, SimulationBar.qml,
    RenderDebugPanel.qml, LogPanel.qml, ScenarioSettings.qml, ConnectDriverDialog.qml, dialogs/
```

Shader sources aren't part of the application source tree. They're Visualizer **data**, kept in the repository's `data/` folder ([11 §13](11-renderer.md#13-shaders-location-and-build)):

```
data/shaders/visualizer/
  scene/                           # scene renderer HLSL (avis_add_shaders TARGET AVIS_VISUALIZER_RENDERING)
  include/                         # bindless.hlsli, brdf.hlsli, packing.hlsli, shared_types.hlsli, …
  qt/                              # fullscreen_triangle.vert, fullscreen_texture.frag (qt_add_shaders)
```

## 3. Viewport integration: QQuickRhiItem + shared handles

### 3.1 Why two devices
Qt Quick renders with its own QRhi D3D12 device on the Qt render thread. The Avis renderer keeps its own device, queues (including async compute and copy) and frame pacing. Separate queues and fences keep them decoupled, so a slow scene frame never stalls the UI. Frames cross over as **shared textures** synchronised by **shared fences**.

> **One device object in practice** (found in task 12.3): D3D12 devices are singletons per adapter in a process, so the Avis RHI's `D3D12CreateDevice` on Qt's adapter returns Qt's device. The decoupling still holds, because it comes from the queues and fences, and the shared-handle protocol works unchanged. What follows from it:
> - **The debug layer and GPU-based validation must be on before Qt creates its device.** Turning either on later removes every device in the process (`DXGI_ERROR_DEVICE_RESET`). `main.cpp` applies `render.debug_layer` and `render.gpu_validation` first, through `ui/viewport_renderer.cpp`, and the RHI's own calls then change nothing (§3.2 step 1).
> - The RHI's info-queue callback and storage filter, and its live-object report at shutdown, cover Qt's objects too.
> - A device removal affects both sides at once.
> - A test standing in for Qt (12.4) uses a second device on the same adapter, which is the same device object with its own queue and fences, as Qt is. A device on another adapter can't stand in: a texture shared with `D3D12_HEAP_FLAG_SHARED` opens only on its own adapter.

### 3.2 Setup sequence
1. `main.cpp` calls `QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D12)` **before** creating the window. It also turns on the D3D12 debug layer and GPU-based validation when the `render` settings ask for them, before Qt's device exists (§3.1).
2. When `QQuickWindow::sceneGraphInitialized` fires, read Qt's adapter from `QRhi::nativeHandles()` → `QRhiD3D12NativeHandles { dev, minimumFeatureLevel, adapterLuidLow, adapterLuidHigh, commandQueue }`. It's emitted on the render thread; the LUID goes to the controller on the GUI thread with a queued call.
3. Create the Avis `rhi::render_hardware_interface` with `adapter = {adapterLuidLow, adapterLuidHigh}` ([09 §4.5](09-rhi.md#45-adapter-selection)). Both devices are now on the same GPU, which shared NT handles require. `rendering::render_system` creates the RHI and the graphics objects of §3.4 in one call; its first failure, with the step that failed, is the startup error of §7.
4. The renderer creates the **shared frame ring** (§3.3) at the viewport's pixel size and exports the handles.
5. The `viewport_renderer` (Qt render thread) opens the handles on Qt's device: `ID3D12Device::OpenSharedHandle` for the textures and the fences. It wraps each texture with `QRhiTexture::createFrom({ quint64(resource), D3D12_RESOURCE_STATE_COMMON })`.

> Steps 2 and 5 are the **only** places native D3D12 types appear in the Visualizer. They're isolated in `ui/viewport_renderer.cpp` and use Qt's public native-handle API, not Avis RHI internals. The Avis side sees only the opaque `rhi::shared_handle` ([09 §13](09-rhi.md#13-cross-device-sharing-rhisharedh)).

### 3.3 Shared frame ring
- **3 slots**, each a shared `RGBA8_UNORM` texture (sRGB-encoded values) at viewport resolution.
- Two shared fences, both created by Avis and opened by Qt:
  - `produced` (signalled by the Avis graphics queue): value `n` means frame `n` has been written into its slot.
  - `consumed` (signalled by Qt's command queue): value `m` means Qt has finished sampling frame `m`.
- Shared CPU state (atomics, written by one side each):

| State | Written by | Meaning |
|---|---|---|
| `latest_completed` = `{frame n, slot i}` | Avis frame chain | The newest frame whose `produced` signal has **completed** on the GPU |
| `picked[i]` | Qt render thread | Qt's last **use** of slot `i`: a count of Qt's renders that sampled a slot (0 = never). `consumed` carries these uses (see *As implemented* below). |
| `showing` | Qt render thread | The slot Qt's most recent `render()` sampled |

**Protocol:**

```mermaid
sequenceDiagram
    participant avis as Avis frame chain and queue
    participant slot as Shared slot i (any free slot)
    participant qt as Qt render and GUI threads
    avis->>avis: shared_slot_free(): pick slot i ≠ showing with consumed ≥ picked[i] (CPU check, never a GPU wait)
    avis->>slot: Render frame n, present_to_shared (final layout COMMON)
    avis->>avis: queue.signal(produced, n)
    avis->>avis: when_reached(produced, n) completes, then latest_completed = {n, i}
    avis->>qt: viewport_item update(), queued to the GUI thread, coalesced
    qt->>qt: render() reads m = latest_completed, sets picked[i] = m and showing = i
    qt->>qt: commandQueue Wait(produced, m), already complete, costs nothing
    qt->>slot: Draw fullscreen textured triangle into the QQuickRhiItem color buffer
    qt->>qt: afterFrameEnd: commandQueue Signal(consumed, m)
```

1. **Avis never waits on Qt on the GPU.** Slot reuse is gated on the CPU by `shared_slot_free()` (§3.4). A slot `i` is free when it isn't `showing`, and either `picked[i] == 0` or `consumed ≥ picked[i]`. If no slot is free yet, the sender completes later through `rhi::sharing::when_reached(consumed, picked[i])`, completed by the RHI's completion driver (§3.4) ([09 §13](09-rhi.md#13-cross-device-sharing-rhisharedh)). No thread and no queue blocks.
2. **Avis picks any free slot**, not `n % 3`, preferring the one released longest ago.
3. **Qt only sees completed frames.** `latest_completed` is set only once the frame chain has observed `produced ≥ n` complete. Qt's GPU `Wait(produced, m)` is kept for correctness but is always already satisfied, so a slow Avis frame never delays Qt's UI frame. The cost is about one frame of extra viewport latency.
4. **`consumed` is signalled after Qt's submission.** `QQuickRhiItemRenderer::render()` runs before Qt executes its command list in `QRhi::endFrame`, so a `Signal` there would land too early. `viewport_renderer` connects to `QQuickWindow::afterFrameEnd` with `Qt::DirectConnection` (it's emitted on the render thread after `endFrame`) and issues `commandQueue->Signal(consumed, m)` there, through `QRhiD3D12NativeHandles::commandQueue`. Verified on Qt 6.11.1 (task 12.2, below), so this direct path is the one used.
   - **Fallback**, kept in case a later Qt or another render loop stops emitting `afterFrameEnd` after submission: in the *next* `render()`, signal `consumed` for the *previous* pick before recording. That's one frame later, and the last pick stays pinned while Qt is idle, which is harmless with three slots (rule 6).
   - **The slot goes back in `COMMON`.** After sampling, Qt's D3D12 backend tracks the texture in `PIXEL_SHADER_RESOURCE` and leaves it there, but Avis writes the next frame from `COMMON`. So at the end of `render()`, `viewport_renderer` records a native transition to `COMMON` on Qt's command list (between `beginExternal` and `endExternal`), then tells Qt with `QRhiTexture::setNativeLayout(D3D12_RESOURCE_STATE_COMMON)`. In its next frame, Qt transitions the texture from `COMMON` again. The `consumed` signal follows this transition, since both are in the frame's submission.
5. **Avis wakes Qt.** `QQuickRhiItem` re-renders only after `update()`. When `latest_completed` advances, the frame chain posts `viewport_item->update()` to the GUI thread with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`. An `update_pending` atomic, cleared by `render()`, keeps at most one call in flight.
6. **No deadlock, and no stall when Qt stops rendering.** Qt holds at most two slots: the one it's `showing` and one still being sampled by its previous in-flight frame. So at least one slot is always free for Avis. When Qt stops rendering (window minimised or occluded, or nothing to update), its last pick simply stays pinned, and Avis alternates between the other two slots at its reduced rate (§3.4).
7. **The Qt side shows the newest completed frame.** If none is new, it re-shows the last one (no stall).
8. **Resize** without `wait_idle`: the item reports its new pixel size to the renderer (debounced 100 ms). The frame chain creates a new ring at the start of its next frame and publishes it with a generation number. The Qt side re-imports when the generation changes. The old ring is destroyed once `consumed` has reached every `picked[i]` of that ring, using the same CPU check and `when_reached` sender.

> **Verified on Qt 6.11.1** (task 12.2, `tests/visualizer_ui/test_qt_interop.cpp`, run by `AVIS_VISUALIZER_UI_TESTS` so that a Qt upgrade re-checks it), with the threaded render loop on Windows and Qt's D3D12 debug layer on:
> - `afterFrameEnd` is emitted on the render thread after `QRhi::endFrame` has submitted. Each frame, `render()` writes the frame number into a buffer from Qt's command list; `afterFrameEnd` then signals a fence and makes Qt's queue wait on a CPU-opened gate before signalling `consumed`. When that fence completes, the number is already written in 100 of 100 frames. The same check from `afterRendering`, which Qt documents as running before submission, sees it in none, so the check can tell the two apart.
> - `QRhiTexture::createFrom({ resource, D3D12_RESOURCE_STATE_COMMON })` accepts a texture created by the Avis RHI and opened with `OpenSharedHandle`. Qt samples exactly what Avis cleared it to, after a GPU `Wait(produced, 1)` on its queue.
> - With the hand-back to `COMMON` of rule 4, both debug layers stay free of errors.

> **As implemented** (task 12.4, `rendering/ring_protocol.h`, `rendering/shared_frame_ring.h`). The protocol above, with these changes, which a model check of 100,000 random Avis and Qt operations covers:
> - **`consumed` counts Qt's uses, not frame numbers.** Each `render()` that samples a slot gets a new use `u` from `ring_protocol::qt_pick()`; Qt stores it in `picked[i]` and signals `consumed` with `u`. With frame numbers, a frame Qt re-shows (rule 7) would already have `consumed` at its number from the first render, and the slot would look free while Qt still samples it. Qt's GPU `Wait(produced, m)` still uses the frame number.
> - **Avis's own slots are busy too.** A slot is free when it isn't `showing`, isn't Avis's own (reserved by the gate, bound to a frame whose `produced` signal isn't complete, or holding `latest_completed`, which Qt may pick at any moment) and has `consumed ≥ picked[i]`. With two frames in flight this can be all three slots for a moment, so rule 6 holds only for Qt's two: the gate then waits on **`produced`** for Avis's oldest frame in flight, and before each decision publishes every bound frame that `produced` shows complete, which frees the slot it replaces. Otherwise it waits on `consumed` for the oldest use Qt still holds; that value is always one Qt has already submitted.
> - **Qt's pick re-checks.** `qt_pick` writes `picked[i]` and `showing`, then re-reads `latest_completed`; if Avis published a newer frame meanwhile, it picks that one. Avis reads `showing` and `picked` after writing `latest_completed`, so one of the two sides always sees the other.
> - **"Released longest ago"** is the slot holding the oldest frame: Qt always picks the newest, so it releases slots in frame order.
> - **Resize (rule 8).** `shared_frame_rings` keeps the current ring and the retired ones. Qt calls `qt_attach()` before importing a ring and `qt_detach()` when it moves to a newer generation; the detach returns one more use, which Qt signals on the old `consumed` after that render. A retired ring is released once `produced` has reached its last frame and either Qt never attached (a compare-exchange makes that final) or `consumed` reached the detach. Without the detach's signal, the release would wait on a fence nothing signals any more.
> - The gate and the release are one sender pattern (`detail::ring_wait_sender`): try, otherwise `rhi::sharing::when_reached` on the value the protocol names, then try again. A stop request ends the wait with `set_stopped`, and a slot taken by the gate but not signalled goes back to the ring when its `ring_slot` is dropped.

> **As implemented** (task 12.5, `ui/viewport_item.h`, `ui/viewport_renderer.cpp`), on the Qt side:
> - **The item reports its colour buffer's size** (`effectiveColorBufferSize`, item size × device pixel ratio) to `shared_frame_rings::request_resize`. The first size goes out at once; later ones after holding for 100 ms (rule 8's debounce).
> - **Rule 5's wake** is `viewport_item::make_wake()`: a function that may be called from any thread and posts `update()` to the GUI thread, with the application object as the context, so a call after the item is gone does nothing.
> - **Resize without a flash.** The renderer attaches to a new generation and opens it at once, but keeps drawing the previous ring until the new one has a published frame; then it detaches from the old one.
> - **Opened objects outlive their last use.** What Qt opened for a ring is kept until that ring's `consumed` reaches the detach's use: `createFrom` doesn't own the resource. When the scene graph destroys the renderer, it detaches from its rings, signals, and waits for Qt's queue before releasing them.
> - **The blit is upright.** `qt/fullscreen_triangle.vert` maps texture row 0 to the top of the target. On D3D12, clip-space y points up while texture v = 0 is the top row; the earlier shader drew frames upside down.

### 3.4 Frame pacing
The Avis renderer has **no dedicated render thread**. Frames are produced by a sender chain ([17 §4](17-graphics-frames-and-transfers.md#4-graphicsframe_schedulerh)) owned by `frame_loop`, inside an `async_scope`:

```text
when_all(begin_frame(), next_frame_tick(), shared_slot_free())
  → continues_on(parallel scheduler) → prepare (gpu_profiler::prepare_frame, extract snapshot, interpolate, residency uploads + upload_queue::flush, build + compile graph)
  → let_value: render_graph::execute (bulk recording, submit)
  → present_to_shared + signal produced fence → end_frame
  → re-arm the next frame (unless stop was requested)

in parallel, per frame: when_reached(produced, n) → publish latest_completed → post update() to the item
```

- `next_frame_tick()` is an `asio::steady_timer` sender on the IO context. It paces frames at the display refresh rate reported by the window's screen, or at `render.maximum_fps`, and at 5 fps when the window is minimised or occluded.
- `shared_slot_free()` is the **CPU-side gate** of the interop ring (§3.3, rule 1). It completes with a free slot index: inline when one is free (the normal case), otherwise through `when_reached(consumed, …)`, which completes from the RHI's completion driver (below). It never issues a GPU wait, so Qt not rendering can't stall the Avis queues.
- Publishing a frame to Qt is a separate small sender per frame (`when_reached(produced, n)`), spawned into the same async scope. The next frame doesn't wait for it.
- No stage blocks a thread: GPU completions resume the chain from the completion driver, timers from IO completions, and CPU work runs on parallel workers.
- **Driving GPU completions.** The RHI owns no thread ([09 §5.2](09-rhi.md#52-driving-completions)). A `QWinEventNotifier` on `rhi::completion_driver::completion_event()`, in Qt's GUI event loop, calls `poll_completions()` whenever the event fires (D-93). That completes every GPU wait of the Visualizer: `begin_frame` (which doesn't drive the RHI itself, D-94), `shared_slot_free()`, the `when_reached` publishes, uploads and readbacks, and it frees destroyed objects. It covers waits on Qt's `consumed` fence while Avis renders nothing. Completions run on the GUI thread, so every continuation that does real work moves to the parallel scheduler with `continues_on` (each stage above already starts with one).
- **Who holds the RHI.** The renderer creates the `rhi::render_hardware_interface` and is its only holder (D-96): `rendering::render_system`, which the controller creates once Qt's adapter is known (§3.2). It builds the views each component takes ([09 §4.3](09-rhi.md#43-subsystem-views)): `completion_driver` for the notifier; `synchronization` and `transient_objects` for the frame scheduler; `transient_objects` and the queues for the GPU profiler; `resources`, `command_lists` and the queues for the render graph, which also gets the frame scheduler and the profiler; `memory` and `resources` for its transient pool; and the upload queue, readback queue and pipeline cache get theirs.
- The chain keeps running while the simulation is paused, so camera motion stays fluid.
- Errors (device lost) arrive on `set_error`, and `frame_loop` turns them into the fatal-error path (§7). The exception is `transient_memory_exhausted` from `render_graph::execute`: the frame's upload memory or descriptors ran out, so `frame_loop` ends the frame with no submissions, skips it and re-arms the next one ([17 §4](17-graphics-frames-and-transfers.md#4-graphicsframe_schedulerh), D-98). The log warning names the budget to raise. Stopping requests stop on the scope's stop source; the chain finishes its current frame and completes with `set_stopped`. A frame stopped before anything was submitted, including one whose token is still inside the opening `when_all`, ends with no submissions: the dropped token ends itself ([17 §4](17-graphics-frames-and-transfers.md#4-graphicsframe_schedulerh), D-102). The controller then waits for the scope to drain at shutdown. After that the controller destroys the notifier, and `render_system` waits for the GPU (`render_hardware_interface::wait_idle`, D-93, which also completes the senders still waiting on it), destroys the graphics-layer objects in the reverse creation order (renderer, shaders, pipeline cache, root signature registry, readback and upload queues, render graph, transient pool, GPU profiler, frame scheduler), then the RHI.

> **As implemented** (task 12.7, `rendering/frame_loop.h`):
> - **One frame at a time on the CPU**, up to F on the GPU (`begin_frame` waits for frame N − F). Each frame's tail spawns the next into the scope.
> - **Ending a frame:** `end_frame` with the frame's submissions, then `produced` is signalled on the graphics queue after `present_to_shared`. The snapshot is held until the frame is submitted.
> - **Without a ring** (no viewport size yet), the chain only ticks.
> - **Retired rings** are destroyed at the start of a frame (`destroy_released`) while no publish is in flight, not through a spawned `when_released`. The chain ticks at 5 fps or more, so polling is enough, and a publish completing on the completion driver's thread can't race with destroying its ring.
> - **Tick period:** `render.maximum_fps` replaces the refresh rate when it's set, 60 Hz until the window reports one, and 5 fps while the window is minimised, hidden or not exposed. Ticks are timed from the previous tick.
> - **GUI → renderer** is a set of setters on `frame_loop` behind one mutex. The pick, inspector and screenshot requests are one-shot. The viewport size goes through the rings (§3.3).
> - **Renderer → GUI** is a `frame_sink` interface, implemented on the GUI thread by `ui::renderer_bridge`. Its debug information owns copies of the graph's passes and resources.
> - **Errors other than device loss and exhausted transient memory** (a frame that can't be built) are counted and logged at most once a second, and the chain goes on.
> - **`stop()` polls the RHI's completions itself** while it waits for the scope to drain, since the GUI thread that normally drives them is the one waiting.

## 4. Threads (Visualizer view)
See [01 §4.1](01-system-architecture.md#41-visualizer). Communication paths:

| From → To | Mechanism |
|---|---|
| GUI → simulation | `command_queue<simulation_command>` (MPSC) + condition-variable wake. Carries both simulation and presentation commands (§4.1). |
| Zenoh → simulation | Lock-free queues of replies and scene stream messages, drained by `scene_stream` and the coordinator |
| Simulation → GUI | Queued Qt signals (`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`) carrying value types: tree diffs, the selected entity's values with its update stamp and world bounds at up to 20 Hz, the follow target's bounds, and scene statistics at 2 Hz ([08 §7](08-scene-management.md#7-ui-synchronisation)) |
| Simulation → renderer | `snapshot_buffer` ([08 §6](08-scene-management.md#6-simulation--render-synchronisation-snapshots)) |
| GUI → renderer | Latest-value atomics: camera state, viewport size, debug selection, render settings, screenshot requests |
| Renderer → GUI | Render-graph debug info and GPU timings, about 4 Hz via queued signal |
| Renderer → Qt render thread | Shared frame ring |

### 4.1 Presentation commands
UI commands sent to the simulation thread are of two kinds:
- **Simulation commands** drive the clock: start, pause, resume, single step, stop, set speed ([03 §3](03-simulation-core.md#3-clock-state-machine)).
- **Presentation commands** change only what's shown. The simulation thread applies them in any clock state, re-runs the representation where needed, and republishes the snapshot without a step (coalesced to at most one republish per 16 ms, [03 §4.4](03-simulation-core.md#44-presentation-only-republish)).

| Presentation command | Effect before the republish |
|---|---|
| Select / clear selection | Moves the `selected` instance flag (tree, inspector and viewport picking all send this) |
| Entity visibility, category visibility (*View* menu) | Updates `visibility` and therefore the `visible` instance flag |
| Helper toggles (grid, axes gizmo) | Adds or removes the helper instances |
| Representation settings (lidar colour mode, point size, box edge width) | Re-runs the affected representations, which produces new mesh or material versions |
| Labels on or off, per category (*View* menu) | Adds or removes `label` components and their helper entities ([08 §8.1](08-scene-management.md#81-labels)) |
| Trail on or off, per category (*View* menu) or per entity (scene tree context menu) | Adds or removes `trail` components and their helper entities; a new trail starts empty ([08 §8.2](08-scene-management.md#82-trails)) |
| Staleness on or off (*View* menu) | Switches stale entities to or from their dimmed materials ([08 §8.4](08-scene-management.md#84-update-stamps-and-staleness)) |
| `visualization` edits while `stopped` (sun, sky) | Updates the light and `environment` |
| *Reset* | Destroys all Driver entities, so the republished snapshot has only the helpers |

Camera motion, viewport size, debug-blit selection and render settings are **not** presentation commands: they go straight to the renderer as latest-value state (table above) and need no snapshot.

> **As implemented** (task 12.6, `simulation/simulation_thread.h`):
> - **The loop and the thread are separate.** `simulation_loop` does the work for a time it's given (`handle`, `update`, `next_due`), so tests drive it with a made-up clock. `simulation_thread` runs one on a pool thread, fed by `command_queue`. The scene is created on that thread, because `scene` checks its owner thread, so the controller keeps only the asset cache and the snapshot buffer.
> - **Step 7 of the tick sequence runs at the start of the next tick.** `end_tick` sets `previous_world = world`. Run straight after the publish ([08 §9.6](08-scene-management.md#96-tick-sequence)), it gave a republish between two ticks `world_previous == world_current`, against [03 §4.4](03-simulation-core.md#44-presentation-only-republish). Deferred, the next tick sees the same state, and republishes keep the tick's transforms.
> - **Coalescing** opens its 16 ms window at the first change, so a burst shorter than that gives one republish.
> - **The simulation thread applies the `selected` and `visible` flags** after the representation (step 4), on the entity and the helpers that `represent` it.
> - **The follow target is re-posted when its `display_shape` extent changes**, not its world bounds, which change whenever the target turns.
> - **The representation is reached through `representation::scene_representation`**, the interface 12.8's `representation_system` implements.
> - **The GUI side** is `ui::simulation_bridge`, which implements `gui_sink` with queued calls to the GUI thread.
> - **Part A without a Driver** publishes step 0 with the anchor `{0, start, 0}`. With `--demo-scene`, `demo_scene` ticks at 10 Hz as steps committed while running (anchor `{t_{k−1}, now, 1}`).

## 5. Camera
The camera is a **presentation concept**: it isn't part of the simulation state and doesn't affect determinism. Two modes:

| Mode | Controls |
|---|---|
| **Free fly** | Roams through the 3D world. RMB held + WASD/QE to move, mouse to look, Shift ×4 speed, wheel to adjust speed. |
| **Follow** (default while running) | Chase camera on a vehicle: the `primary` vehicle (the vehicle of the vehicle model) by default, or the selected vehicle entity. Offset and lag are configurable (`visualization.camera.follow`), and LMB drag orbits around the vehicle; the orbit offset is kept while following. |

- Following uses the interpolated vehicle pose at render rate, so the chase stays smooth between ticks. Lag is a critically damped spring on the camera position.
- **Follow distance from the target's size.** The configured offset is for a reference radius of 2.5 m (a car). It's scaled by the target's bounding radius ([08 §8.3](08-scene-management.md#83-world-bounds)) divided by that, clamped to 0.5–4×, so a truck and a cyclist are both framed well. The simulation thread posts the target's bounds when the target changes and when its extent changes.
- **Frame selection** (`F`, or double-click in the scene tree) switches to free fly and moves the camera along its current view direction until the bounding sphere of the selection's world bounds fills the view with a 1.2 margin, in 0.3 s with an ease-out. An entity with nothing drawn is framed as a 2 m sphere around its position.
- Switching from follow to free fly keeps the current viewpoint. When the followed entity is deleted, the camera switches to free fly at its last viewpoint.
- **Picking:** clicking the viewport makes the renderer write a 64-bit `picking_id` target (`R32G32_UINT`) in the G-buffer, forward-transparent and overlay passes on that frame, and read back a small window around the cursor ([11 §5.1](11-renderer.md#51-picking)). Opaque geometry, translucent boxes and overlay lines are all pickable. The returned `object_id` maps back to the entity (through `identity`), which is then selected in the tree and inspector.

## 6. Panels and controls

```
┌───────────────────────────────────────────────────────────────────────────────┐
│ File  Edit  Simulation  View  Render  Help                                    │
├──────────────────┬────────────────────────────────────────┬───────────────────┤
│ Scene Tree       │                                        │ Inspector         │
│  ▾ ego_vehicle   │                                        │  identity         │
│     image_00     │              3D viewport               │  transform        │
│     velodyne     │           (QQuickRhiItem)              │  vehicle          │
│  ▸ shadow_vehicle│                                        │  perception       │
│  ▸ perception(37)│                                        │   classification  │
│  ▸ belief (5)    │                                        │   confidence      │
├──────────────────┴────────────────────────────────────────┴───────────────────┤
│ ▶ ❚❚ ⏭ ■  speed [1.0x ▾]  t = 12.340 s  step 617/6000  effective 0.93x  Driver: ● ok │
├───────────────────────────────────────────────────────────────────────────────┤
│ [Log] [Render Debug] [Driver]                                                 │
└───────────────────────────────────────────────────────────────────────────────┘
```

| Panel | Content |
|---|---|
| **Menus** | File: New, Open, Save, Save As, Recent, Screenshot ▸ (viewport, custom resolution…, window with UI), Exit. Edit: Undo, Redo, Scenario Settings. Simulation: Connect Driver, Start, Pause, Step, Stop, Reset, Speed ▸. View: panels, camera mode (free fly, follow), frame selection, helpers (grid, axes), visibility, labels and trails per entity category, stale entities. Render: debug view, AO/TAA/shadows toggles, tonemapper, exposure, statistics overlay (scene statistics, [08 §8.5](08-scene-management.md#85-scene-statistics), next to the representation's reduction level and the GPU timings). Help: About, Open logs folder. |
| **Scene tree** | `scene_tree_model` (QAbstractItemModel) over all entities, with hierarchy, name, category icon and a visibility toggle. Stale entities get a marker when staleness is on. Its context menu has *Frame selection* and *Show trail*. Has a search filter, and large groups are lazily populated. Visualizer helpers for labels and trails are hidden from it. Entities appear and disappear as the Driver creates and deletes them. |
| **Inspector** | Read-only view of the selected entity: reflected component values ([08 §3](08-scene-management.md#3-components)) and the Driver's properties grouped by their dotted keys, with units (for example the classification the vehicle model gave the entity, or its estimated dimensions), and when the Driver last created or changed it (*"pose: 1.2 s ago (step 1234)"*, [08 §8.4](08-scene-management.md#84-update-stamps-and-staleness)). Values refresh at 20 Hz while running. |
| **Simulation bar** | Start/Pause/Step/Stop, speed (0.1×, 0.25×, 0.5×, 1×, 2×, 4×, 10×, max), simulation time, step index / `step_count`, effective speed (`s_effective`, [03 §5.1](03-simulation-core.md#51-effective-speed-s_effective)), driver status light (connected, *busy* with the phase and duration from `driver_status` after `long_step_notice` ([02 §6.2](02-communication.md#62-rules)), unresponsive, faulted, disconnected, plus *waiting (debug mode)* and *suspended?* for a Driver in debug mode, [02 §7.1](02-communication.md#71-with-a-driver-in-debug-mode)) with a tooltip |
| **Scenario settings** | A form over the scenario `simulation`, `dataset`, `driver` and `visualization` sections. The data source, frame range and sensors are picked from the connected Driver's catalogue ([02 §5.2](02-communication.md#52-describe)); without a connected Driver the values can still be edited as text but aren't checked. The **frame range** fields show the approximate time next to each frame (from the catalogue's `duration_ns` and frame count) and also accept input in seconds, which is converted to the nearest frame; the file always stores frames ([04 §2.3](04-scenario-and-dataset.md#23-validation)). The exact end time comes back with `step_count` after `load`. **Plug-in parameters** get a form generated from the module's `parameters_schema` (§6.1). Opening the editor sends `rescan`, so the module list and schemas are current. |
| **Render debug** | The render graph's passes (queue colour, GPU ms, culled) and resources (qualified name, format, size, aliased). Selecting a resource adds the debug blit with mode, channel, range, mip/slice and fullscreen/PiP options ([11 §11](11-renderer.md#11-debug-blit-pass)). Also: memory statistics (aliased vs non-aliased). |
| **Log** | Local and driver logs with level filter, logger filter and text search. Faults are highlighted. |
| **Driver** | Connection details, debug-mode indicator, the plug-in catalogue (loadable modules; rejected and pinned plug-ins with their reasons; *"plug-ins locked: run loaded"* while a run is loaded), a **Rescan plug-ins** button ([02 §5.6a](02-communication.md#56a-rescan)), data sources (from `describe`), per-step timing breakdown chart (replay/perception/model/physics/publishing), scene stream rate |

**Selection is shared** by the scene tree, the inspector and viewport picking: one selected entity id lives in a `selection_model`, and all three follow it. Selecting in the tree shows that entity in the inspector, and picking in the viewport selects it in the tree (expanding its parents). When the Driver deletes the selected entity, the selection clears.

### 6.1 Editing workflow
- Editing is available only in `stopped`. Controls are disabled otherwise, and a banner explains why.
- Every edit is a `QUndoCommand` on the scenario model. The dirty flag is shown in the title.
- *Start* validates the scenario ([04 §2.3](04-scenario-and-dataset.md#23-validation)). Errors open the problems list, and clicking an entry jumps to the field. The Driver's `load_scenario` diagnostics (parameter-schema errors and warnings with their paths) land in the same list.
- **Plug-in parameter forms:** when the selected module publishes a `parameters_schema` ([05 §5.8](05-driver.md#58-parameter-schema)), its parameters are edited in a generated form. Each field shows its type-appropriate control, default, range, unit and description, and is checked as you type. Values equal to the default are still written to the scenario, so the file is self-describing. A module without a schema, or YAML the schema doesn't cover, falls back to the raw YAML editor.
- **Rebuilding plug-ins:** stopping the run unloads the plug-ins, so their DLLs can be rebuilt; *Start* rescans and loads the new build ([05 §5.4](05-driver.md#54-host-side)).

### 6.2 Driver connection
- *Connect Driver* runs Zenoh **scouting** (the Visualizer initiates it, [02 §2](02-communication.md#2-zenoh-topology-and-discovery)), connects to the peers that reply and sends `describe` to each. The dialog lists every Avis Driver found with its id, host, version, plug-ins and whether it's already bound to another session. *Refresh* scouts again.
- Choosing a Driver runs `connect`. The status shows the Driver id, host name, version and plug-ins. *Start* requires a connected Driver with no run loaded.
- At startup the Visualizer scouts once in the background and reconnects the last used Driver if it's available and unbound.

### 6.3 Screenshots
Available in every clock state, from *File → Screenshot* and a toolbar button over the viewport:

| Command | Shortcut | What it captures |
|---|---|---|
| *Viewport* | F12 | The 3D viewport at its current pixel size, rendered by the Avis renderer ([11 §12.1](11-renderer.md#121-screenshot-capture)). No Qt UI. The debug blit is included only if *Include debug view* is ticked. |
| *Custom resolution…* | Shift+F12 | A dialog with width and height (presets 1920×1080, 2560×1440, 3840×2160, 7680×4320, or the last used value) and *Include debug view*. Rendered offscreen with 8-sample accumulation ([11 §12.1](11-renderer.md#121-screenshot-capture)). |
| *Window with UI* | Ctrl+F12 | The whole window, panels included, through `QQuickWindow::grabWindow()` on the GUI thread. The viewport shows the frame Qt is currently displaying. The `QImage` is saved as PNG on a parallel worker. |

- When the file is written, a toast shows the file name with *Open* and *Show in folder*. Errors (VRAM limit, disk) appear as a toast and in the log.
- Configuration (`visualizer.yaml` → `screenshots`):
  ```yaml
  screenshots:
    folder: ""                       # empty = %USERPROFILE%/Pictures/Avis
    custom: { width: 3840, height: 2160 }   # last used custom resolution
    include_debug_view: false
  ```
- **Video capture** of the viewport is out of scope for this iteration ([00 §3](00-overview.md#3-scope-of-iteration-1)).

## 7. Error presentation
- Startup capability failure ([09 §4.4](09-rhi.md#44-capability-gate)): a modal dialog naming the missing feature (for example *"Shader Model 6.6 is required; your GPU/driver reports 6.5"*), then exit.
- Startup shader failure (§10): a missing or malformed `manifest.yaml`, a missing blob, a hash mismatch or a missing shader. A modal dialog with the message (file, line and key path for manifest errors) and the log path, then exit (§10.3).
- Lockstep failures ([02 §7](02-communication.md#7-failure-handling)), meaning silence for `communication_timeout` or a fault, not slowness: the simulation pauses, a non-modal banner over the viewport offers *Retry step*, *Reload plug-ins* or *Stop*, and the detail goes to the log. *Reload plug-ins* re-sends `load`, which rescans first. A pinned plug-in (abandoned after an SEH fault) reloads from the same build, and the banner says a Driver restart is needed to pick up a rebuild. With a Driver in debug mode, timeouts and liveliness loss don't pause ([02 §7.1](02-communication.md#71-with-a-driver-in-debug-mode)).
- Unrecoverable render error (device lost): a modal dialog with the log path, then a controlled exit. The scenario is autosaved first if dirty.

## 8. Settings persistence
`visualizer.yaml` ([01 §7](01-system-architecture.md#7-configuration)) holds the render settings, Zenoh configuration (scouting timeout), the last used Driver and recent files. Window geometry and panel layout are kept with `QSettings` (registry), since they're per-user UI state.

## 9. Scene representation
The **scene representation** (`avis::visualizer::representation`, `apps/visualizer/representation/`) is the layer between the Driver-streamed scene and the scene-agnostic renderer ([11 §2.1](11-renderer.md#21-scene-agnostic-boundary)). It runs on the simulation thread at the end of `apply_result` ([03 §4.3](03-simulation-core.md#43-apply_resultk)). For each streamed component it ensures the entity, or helper child entities, carry `mesh`, `material` and `render_options` components. The snapshot then picks those up like any other mesh. It never interprets what an entity *means*: it only looks at display shapes, geometry and sweeps.

| Streamed content | Representation | Draw layer |
|---|---|---|
| `display_shape` `box` | Instance of the shared unit-cube mesh scaled to `extent`, with a translucent material in the entity's colour and opacity (`alpha_mode = blend`), **plus** a child instance of the shared unit-cube **edge line mesh** with an unlit material in the same colour | faces: `world`; edges: `overlay` |
| `display_shape` `vehicle` | The proxy glTF mesh (`data/assets/vehicle_proxy.glb`) scaled to `extent`. Opacity below 1 (for example the shadow vehicle) uses a translucent "ghost" material. | `world` |
| `display_shape` `axes` | Small shared axes line mesh | `overlay` |
| `display_shape` `frustum` | Line mesh built from the horizontal and vertical field of view and range | `overlay` |
| `belief_geometry` chunk | One mesh per chunk, created or rewritten through `asset_cache::create_mesh` / `update_mesh` with the chunk's topology, vertex colours and normals; material from the chunk `style` (colour, opacity, lit or unlit, line width, point size). `points` chunks are progressive point meshes. | `world` or `overlay` from the style |
| `lidar_points` sweep | One progressive point mesh per kept sweep, placed at the sweep's sensor pose; colour by intensity, height, ring or uniform (`visualization.lidar.color_mode`); lit point material with `point_size` | `world` |
| `label` | A child helper entity at the label's anchor with a glyph-quad mesh on the font atlas, plus a background quad; rebuilt only when the text changes, at most every 250 ms. `screen_aligned`, with the style's `maximum_view_distance`, not pickable ([08 §8.1](08-scene-management.md#81-labels)) | `overlay_on_top` |
| `trail` | A helper entity under `helpers` with world-space line meshes in blocks of 128 samples; only the newest and oldest blocks change per tick ([08 §8.2](08-scene-management.md#82-trails)) | `overlay` |
| Stale entity (when enabled) | The entity's instances switch to dimmed variants of their materials (opacity × 0.4, desaturated), created once per material ([08 §8.4](08-scene-management.md#84-update-stamps-and-staleness)) | unchanged |
| World grid and axes gizmo | Visualizer helpers: static line meshes | `overlay` (grid), `overlay_on_top` (axes gizmo) |
| Selection | The `selected` instance flag on the selected entity's instances | — |

- The representation owns a small library of shared meshes (unit cube, unit-cube edges, axes, vehicle proxy) and materials, created once through the asset cache.
- All visual choices that the Driver doesn't specify (edge widths, lidar colouring, which helpers are visible from the *View* menu) live here, so changing how something looks never touches the renderer.
- Configuration (`visualizer.yaml` → `representation`):
  ```yaml
  representation:
    boxes: { edge_width: 2.0 }
    lidar: { maximum_points_per_sweep: 200000 }
    geometry: { budget_mib: 512 }       # CPU-side budget for dynamic meshes (belief geometry, lidar sweeps, trails), §9.1
    labels:
      enabled: true
      maximum_count: 200                # nearest to the primary vehicle first
      font: data/assets/fonts/labels    # atlas and metrics, 1x and 2x (08 §8.6)
      style: { pixel_height: 14.0, maximum_view_distance: 80.0 }
      templates:                        # per entity category; "" = no label
        vehicle: "{name}"
        object: "{name} · {property:perception.speed:.1f}"
        sensor: ""
    trails:                             # per entity category; 0 = off
      vehicle: { duration_seconds: 30.0, minimum_spacing: 0.25 }
      object: { duration_seconds: 0.0 }
    stale: { enabled: false, after_seconds: 2.0, categories: [object] }
  ```

### 9.1 Geometry budget and eviction
The representation creates all dynamic geometry, so it is the place that knows what matters and enforces the budget. The renderer stays agnostic ([11 §3.6](11-renderer.md#36-gpu-residency)): it only uploads what it receives, and sizes its GPU arena at about 1.5 × this budget, so superseded versions fit while they retire.

- The budget counts the vertex and index bytes of the **current** version of every dynamic mesh the representation owns, trails included; trails are thinned with belief geometry, in step 3. Label meshes are small and capped by `labels.maximum_count` instead. The shared static meshes (unit cube, edges, axes, vehicle proxy) are excluded.
- Before a `create_mesh` / `update_mesh` that would exceed `representation.geometry.budget_mib`, the representation frees space in this order:
  1. **Drop the oldest lidar sweeps first.** Each lidar sensor's kept sweeps are lowered below their `retention` count, oldest first, down to a minimum of the newest sweep per sensor. The `lidar_points` component keeps its configured retention, and the reduction is undone as soon as there's room again.
  2. **Shrink progressive point meshes.** Progressive meshes (lidar sweeps, belief point chunks) are stored in random order, so their vertex lists are truncated to a prefix, largest mesh first, down to 25% of their points. That's still a uniform subsample, so the picture thins out evenly instead of losing regions.
  3. **Skip new geometry.** If that's still not enough, the new mesh or chunk isn't created (an update keeps the previous version), and a rate-limited warning goes to the log and the statistics overlay.
- The current reduction level (sweeps dropped, prefix fraction, chunks skipped) is shown in the statistics overlay, so a thinned-out picture is never mistaken for missing Driver data.
- These choices are presentation only: they never change what the Driver sends, so determinism is unaffected.

## 10. Shader loading

The Visualizer owns where its shaders come from. The graphics layer loads a `graphics::shader_library` from a `graphics::shader_manifest` and creates pipelines from it through its pipeline cache [18](18-graphics-shaders-and-pipelines.md); the RHI only sees bytecode. Neither knows the build's `manifest.yaml` (D-73). The Visualizer reads and parses that file itself, with its own parser. This section was designed as 12a and folded in here once task 11 had built it.

| Feature | Namespace | CMake target | Location |
|---|---|---|---|
| Manifest parser | `avis::visualizer::rendering` | `AVIS_VISUALIZER_RENDERING` (linked by `AVIS_VISUALIZER` and `AVIS_TESTS`) | `apps/visualizer/rendering/shader_manifest_parser.{h,cpp}` |
| Shader ids, directory and startup load | `avis::visualizer::rendering` | `AVIS_VISUALIZER_RENDERING` | `apps/visualizer/rendering/renderer_shaders.{h,cpp}` |

The parser depends only on `AVIS_LIB` (`avis::error`, `graphics::shader_manifest`, [18](18-graphics-shaders-and-pipelines.md)) and yaml-cpp. It stays in the Visualizer, the only application with shaders. If a second application (a tool, an offline renderer) loads the build's shaders, the parser moves to a small shared application library. The library stays free of the format either way.

### 10.1 The manifest format
`avis_add_shaders` writes `manifest.yaml` next to the blobs, through `dt shader-manifest` from `tools/data-trace` ([13 §3](13-build-and-quality.md#3-shader-build-pipeline)). The build writes **format 2**, which adds `bindings` per entry ([18 §6](18-graphics-shaders-and-pipelines.md#6-reflection-data-sources)). The parser also reads format 1.

```yaml
avis_shader_manifest: 2
shaders:
  - id: lighting.main.cs
    file: lighting.main.cs.cso
    stage: cs
    entry: main
    defines: []
    xxh3: 8e4016d2ad80d9e1
    bindings:
      constant_buffers:
      - {register: 0, space: 0, size: 64}
      buffers: []
      samplers:
      - {register: 1, space: 0}
```

| Key | Required | Type and rule | Maps to `graphics::shader_manifest_entry` |
|---|---|---|---|
| `avis_shader_manifest` (top level) | yes | integer, `1` or `2` | — (checked) |
| `shaders` (top level) | yes | sequence, may be empty | `shader_manifest::entries`, in file order |
| `id` | yes | non-empty string, unique in the file | `id` |
| `file` | yes | relative path: no root, no drive, no `..` component | `blob` |
| `stage` | yes | `vs`, `ps` or `cs` (the DXC target prefixes of 13 §3) | `stage`: `vertex`, `pixel`, `compute` |
| `entry` | no | string | `entry` (informational) |
| `defines` | no | sequence of strings | `defines` (informational) |
| `xxh3` | yes | string of exactly 16 hexadecimal digits, case-insensitive, no `0x` | `content_hash` |
| `bindings` | format 2: yes; format 1: an error | `constant_buffers` (`register`, `space`, `size`), `buffers` (`kind`, `register`, `space`), `samplers` (`register`, `space`) | `bindings` |

- **The parser is strict**, because the manifest is generated. An unknown key is an error at the top level, in an entry and in `bindings`, and so is a version other than 1 or 2. An unknown key means the writer and the parser drifted apart, so an additive change bumps the format.
- **`xxh3` is required.** The loader's integrity check is the only protection against a stale or half-copied deployment. Tests that want no hashes build a `graphics::shader_manifest` directly instead of writing YAML.
- **`file` is checked for path traversal**, because the loader joins it to the shader directory.
- **Duplicate ids** are reported with both line numbers. `shader_library::create` would reject them too, but without positions.
- The parser doesn't check that ids follow `<name>.<entry>[.<permutation>].<stage>`, or that the file name matches the id. The build guarantees both, and nothing at run time depends on them.

### 10.2 The parser
```cpp
namespace avis::visualizer::rendering
{
    // Errors are error_domain::parse: "<source_name>:<line>:<column>: <key path>: <message>", for example
    // "manifest.yaml:14:12: shaders[1].stage: expected vs, ps or cs, got 'gs'". Pure: no file access, no logging,
    // no global state; safe on any thread.
    result<graphics::shader_manifest> parse_shader_manifest(std::string_view yaml_text,
                                                            std::string_view source_name = {});
}
```

- **One function, no class**, and **the first error wins**. A malformed manifest means a broken build, and one precise message helps more than a list.
- **Positions** follow `runtime_configuration::parse` ([14 §3](14-library-runtime.md#3-configuration)): yaml-cpp's 0-based marks become 1-based `line:column`, and `0:0` without a mark. Key paths are `shaders[<index>].<key>`.
- yaml-cpp's exceptions become the error with their mark. Every value is read by small typed readers that return `result` and carry the key path. The hash is parsed with `std::from_chars` after a length check. Paths are UTF-8 (`std::filesystem::path(std::u8string_view)`), so non-ASCII file names survive on Windows.

### 10.3 Startup
The renderer starts after the RHI exists (§3.2 step 3) and before the first frame. Startup is the sender `rendering::load_renderer_shaders(directory)` (`renderer_shaders.h`), then `scene_renderer::create` ([11 §2.4](11-renderer.md#24-renderer-interface)):

1. **Resolve the directory** with `resolve_shader_directory(settings, executable_directory)`: `render.shader_directory` when set (relative to the executable's directory), otherwise `<executable directory>/data/shaders/visualizer` ([11 §13](11-renderer.md#13-shaders-location-and-build)).
2. **Read** `manifest.yaml` with `io::read_file`.
3. **Parse** on a parallel worker with `parse_shader_manifest`, with the path as `source_name`.
4. **Load** with `graphics::load_shader_library(manifest, directory)` ([18 §4](18-graphics-shaders-and-pipelines.md#4-graphicsshader_loadingh)): it reads every blob, checks each `xxh3` and creates the library.
5. **Create the pipelines.** `scene_renderer::create` creates the base pipeline sets in a `bulk` through `graphics::pipeline_cache` ([18 §11](18-graphics-shaders-and-pipelines.md#11-graphicspipeline_cacheh)).

`renderer_shaders.h` also holds the `constexpr` ids of the renderer's compute and fullscreen shaders (`shader_ids`). The pipeline variant registry builds the ids of the scene passes' vertex and pixel shaders from a pipeline set's key.

**Errors.** Any failure is a startup error (§7): a modal dialog with the message and the log path, then exit.

| Failure | Message shape |
|---|---|
| Manifest missing or unreadable | IO error with the directory: *"manifest.yaml not found in <directory>; was the Visualizer built and deployed with its shaders?"* |
| Malformed manifest | Parse error: *"<path>:14:12: shaders[1].stage: expected vs, ps or cs, got 'gs'"* |
| Blob missing or hash mismatch | From `load_shader_library`, naming the entry's id and file |
| Duplicate id or hash collision | From the parser (with lines) or `shader_library::create` (`duplicate_shader_id`) |
| Missing shader for a pipeline | `shader_not_found` from `graphics::pipeline_cache`, naming the pipeline's debug name |

*As implemented* (tasks 11.22 and 12.3), until `frame_loop` exists:
- `rendering::render_system::create` runs these steps once Qt's adapter is known (task 12.3, §3.2), waiting for the load on the GUI thread. The failures of the RHI and of every graphics object are startup errors too.
- The message is prefixed with the step that failed, for example *"the renderer's shaders could not be loaded: manifest.yaml not found in …"*, and logged in full.
- `main.qml` shows it in a `QtQuick.Dialogs` `MessageDialog`, then exits with code 1.
- The log is `visualizer.log` next to the executable until `visualizer.yaml`'s `logging` section is read.

### 10.4 Testing
`tests/visualizer/rendering/test_shader_manifest_parser.cpp` (`[renderer][unit]`):
- hand-written cases in formats 1 and 2 parse into the expected entries (stages, hashes, defines, bindings, order), and an empty `shaders` sequence gives an empty manifest;
- each rule of §10.1 fails with the expected key path and position, unknown keys included, and a duplicate id names both lines;
- malformed YAML reports yaml-cpp's position;
- **build round trip:** the manifest the build wrote for the renderer's shaders parses, so the build's writer and this parser can't drift apart.

`[renderer]` tests also load every id of `renderer_shaders.h` with `load_renderer_shaders` from the built directory, and check that a missing manifest is an IO error naming the directory ([tasks/11](../tasks/11-renderer.md) 11.1).
