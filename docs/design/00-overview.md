# Avis Design Documents — Overview

| | |
|---|---|
| Status | Draft, design iteration 1 |
| Source | [`docs/idea.md`](../idea.md) |
| Nature | **Target design.** The code moves toward this design; where existing code differs, the design wins unless a document says otherwise. |

## 1. Purpose

Avis is a virtual 3D environment for **developing and demonstrating** autonomous-driving vehicle models. Evaluation (scoring, metrics, regression runs) is explicitly deferred to a later iteration.

This iteration builds a two-application system:

- **Visualizer**: the application the user works in to open, edit and save a simulation run, connect to a Driver, control the simulation and view the results. It owns the virtual simulation clock and renders, in real time with a DirectX 12 deferred/forward renderer embedded in a Qt Quick UI, a scene that the Driver fills through messages. It is agnostic to the vehicle model and reads no datasets.
- **Driver**: the vehicle-model host. It replays KITTI-360 sensor data, runs a perception module and the user's vehicle model, simulates a shadow vehicle with a kinematic model and a physics engine, and streams the changes of the world it knows about to the Visualizer. It advances in lockstep with the Visualizer.

Both applications are built on the **Avis library**, a set of generic components: library runtime, thread pool, task system, async IO, scene management, RHI and render graph.

## 2. Document map

| # | Document | Scope |
|---|---|---|
| 00 | [Overview](00-overview.md) | This document: scope, conventions, glossary, dependencies |
| 01 | [System architecture](01-system-architecture.md) | Processes, deployment, data flow, threading, error policy, configuration |
| 02 | [Communication & lockstep](02-communication.md) | Zenoh topology and scouting, key expressions, message framing, message catalogue (control and scene stream), lockstep protocol, failure handling |
| 03 | [Simulation core](03-simulation-core.md) | Virtual clock, fixed timestep, time scaling, slowdown, interpolation, determinism rules |
| 04 | [Scenarios & KITTI-360 data](04-scenario-and-dataset.md) | Scenario YAML schema, editing rules, KITTI-360 integration, the Avis sequence pack format and packer tool |
| 05 | [Driver application](05-driver.md) | Driver architecture, plug-in interfaces, ground-truth perception oracle, shadow vehicle, fault isolation |
| 06 | [Thread pool](06-thread-pool.md) | `avis::threading::thread_pool`: pool of long-running threads, stop/join lifecycle |
| 07 | [IO layer](07-io-layer.md) | Async file IO with stdexec + ASIO, error propagation |
| 08 | [Scene management](08-scene-management.md) | ECS with EnTT, components, scene graph, snapshots, glTF assets |
| 09 | [RHI](09-rhi.md) | DirectX 12 hardware interface: devices, queues, memory, bindless, barriers, shaders |
| 10 | [Render graph](10-render-graph.md) | Build / compile / execute, async compute, aliasing, multi-view, multi-threaded recording |
| 11 | [Scene renderer](11-renderer.md) | Clustered deferred + forward pipeline, CSM, GTAO, TAA, tonemapping, point clouds, debug blit |
| 12 | [Visualizer application](12-visualizer.md) | Qt Quick UI, QQuickRhiItem interop with shared handles, camera, panels, editing |
| 13 | [Build, tooling & quality](13-build-and-quality.md) | CMake layout, dependencies, shader build, Python tools, logging, testing |
| 14 | [Library runtime](14-library-runtime.md) | `avis::runtime`: configuration, default services and their static access, initialisation and shutdown order |
| 15 | [Task system](15-task-system.md) | `avis::threading::task_system`: work-stealing task execution on pool threads, stdexec scheduler and `bulk` |
| 99 | [Decisions & open questions](99-decisions-and-open-questions.md) | Decision log, assumptions made on the user's behalf, items still to confirm |

Suggested reading order: 00 → 01 → 02 → 03 → 04 → 05 for the system; 14 → 06 → 15 → 07 → 08 → 09 → 10 → 11 → 12 for the library and rendering.

## 3. Scope of iteration 1

### In scope
- Windows 11 only (x64). Earlier Windows versions are not supported.
- C++23, CMake, MSVC toolchain. Python (≥ 3.14, `uv`-managed) for tools.
- Two applications, Visualizer and Driver, communicating over Zenoh with a custom binary message format. The Visualizer discovers Drivers with Zenoh scouting. They may run on the same machine or on separate machines in a LAN.
- Deterministic, fixed-timestep, lockstep simulation driven by the Visualizer.
- KITTI-360 as the sensor-data source, in **open-loop replay**: the ego vehicle follows the recorded trajectory.
- A perception plug-in interface with a ground-truth oracle implementation.
- A vehicle-model plug-in interface. Plug-ins use a **plain C ABI**, so any compiler or language with C FFI works. The shadow vehicle uses a kinematic model plus Jolt Physics.
- Real-time rendering: clustered deferred plus forward transparency, CSM, GTAO (own implementation), PBR, TAA, tonemapping, and a debug blit pass.
- A Driver-streamed scene: it starts empty at the start of a run and builds up from entity, transform, vehicle state, property, belief geometry and lidar sweep messages ([02 §5.8](02-communication.md#58-scene-stream)).
- Scene inspection: entity tree, entity inspector (synchronised selection), free-fly and follow cameras.
- Scenario editing in the Visualizer (data source, sensors, plug-ins, parameters), only while the simulation isn't running.
- Screenshots of the 3D viewport (at viewport or a custom resolution), plus a screenshot of the whole window including the UI ([11 §12.1](11-renderer.md#121-screenshot-capture)).

### Out of scope (explicitly deferred)
- Evaluation, metrics and scoring.
- Rendered (synthetic) sensor simulation. Sensors come from KITTI-360 recordings.
- Live camera images in the Visualizer. Lidar sweeps *are* streamed and shown in the 3D scene ([02 §5.8.6](02-communication.md#586-lidar_sweep)).
- Dataset access from the Visualizer: only the Driver reads sequence packs.
- Closed-loop control of the ego vehicle in the replayed world.
- Rewinding the simulation.
- HDR display output.
- Vulkan or any other graphics API, and the D3D12 Agility SDK.
- Shader hot reload and pipeline libraries.
- Python as an in-application scripting language.
- Re-synchronising a Driver or Visualizer that reconnects mid-run (the scenario must be restarted).
- Video capture of the viewport (deferred to a later iteration).
- Recording a run and replaying it without a Driver (deferred to a later iteration).

## 4. Global conventions

### 4.1 Coordinate systems and units

| Item | Convention |
|---|---|
| Units | SI: metres, seconds, radians, kilograms. Angles in the UI are shown in degrees. |
| World frame | Right-handed, **Z up**, X forward/east, Y left/north. This matches the KITTI-360 world/pose frame, so poses import without axis swizzling. |
| Vehicle body frame | ISO 8855: X forward, Y left, Z up, origin at the rear-axle centre projected to the ground. |
| glTF assets | glTF is right-handed **Y up**. It is converted at import with the fixed rotation `R = RotX(+90°)`, which maps glTF +Y to Avis +Z. |
| View / clip space | Right-handed view space looking down −Z. **Reverse-Z** infinite-far projection, depth format `D32_FLOAT`, clear depth 0, depth test `GREATER_EQUAL`. |
| Time | `std::chrono::nanoseconds` on `int64_t` for all simulation time. Floating-point time is never accumulated. |
| Dataset time | KITTI-360 timestamps are converted to nanoseconds. The **reference timeline** is the `image_00` timestamp of each frame index (`frames.avs`, [04 §4.2a](04-scenario-and-dataset.md#42a-reference-timeline-framesavs)). Simulation time `t = 0` maps to the reference timestamp of the scenario's first frame. Every sensor stream keeps its own timestamps relative to that zero. |

### 4.2 Code conventions
- `snake_case` for types, functions and variables. Member variables have a trailing underscore (`device_`). This matches the existing code.
- Namespaces: see §4.3.
- Headers live in `include/avis/<component>/`, sources in `lib/avis/<component>/`. Include guards follow the `AVIS_<COMPONENT>_<FILE>_H` pattern.
- Formatting follows the repository `.clang-format`.
- Errors: `std::expected<T, avis::error>` for recoverable errors, exceptions for unrecoverable ones (see [01 §6](01-system-architecture.md#6-error-handling-policy)).
- Math: Eigen (`external/eigen-5.0.1`) is used throughout. Single-precision types (`Eigen::Vector3f`, `Eigen::Quaternionf`, `Eigen::Matrix4f`) are used for rendering and scene data. Double precision (`Eigen::Vector3d`) is used for dataset poses and the driver's vehicle state, which are converted to float relative to the scenario origin (see [04 §4.4](04-scenario-and-dataset.md#44-coordinate-frames-and-precision)).

### 4.3 Code placement: namespaces and CMake targets
All Avis **library** features are built into **one CMake target, `AVIS_LIB`**. Features are separated by namespace and directory, not by CMake target. Every design document starts with a *Code placement* table naming the namespace, CMake target and location of each feature it describes. The full map is in [13 §2.1](13-build-and-quality.md#21-library-targets).

| Namespace | Contents | CMake target |
|---|---|---|
| `avis` | Error type, runtime, virtual clock, FSM, enum flags | `AVIS_LIB` |
| `avis::log` | Logging facade | `AVIS_LIB` |
| `avis::threading` | Thread pool, task system (separate classes) | `AVIS_LIB` |
| `avis::io` | Async IO | `AVIS_LIB` |
| `avis::network`, `avis::network::protocol` | Zenoh wrapper, framing, protocol messages | `AVIS_LIB` |
| `avis::simulation` | Simulation time, clock state, scenario model | `AVIS_LIB` |
| `avis::dataset` | Sequence-pack readers | `AVIS_LIB` |
| `avis::scene`, `avis::scene::components` | Scene, scene graph, components, assets, snapshots | `AVIS_LIB` |
| `avis::rhi` | Render hardware interface (`avis::rhi::d3d12` internal) | `AVIS_LIB` |
| `avis::graphics` | Render graph | `AVIS_LIB` |
| `avis::visualizer` | Visualizer application | `AVIS_VISUALIZER` |
| `avis::visualizer::representation` | Scene representation: semantics → meshes and materials | `AVIS_VISUALIZER` |
| `avis::visualizer::rendering` | Scene-agnostic scene renderer | `AVIS_VISUALIZER` |
| `avis::driver` | Driver application, including the oracle | `AVIS_DRIVER` |
| `avis::stepper` | Headless lockstep test CLI | `AVIS_STEPPER` |
| C `avis_` prefix, `avis::sdk` | Plug-in SDK (C ABI + optional C++ wrapper) | `AVIS_DRIVER_SDK` (header-only, outside `AVIS_LIB` on purpose) |

## 5. Glossary

| Term | Meaning |
|---|---|
| **Tick / step** | One fixed-timestep advance of the simulation, of length `delta_time`. |
| **Frame** | One rendered image. Frames and ticks are decoupled. |
| **Lockstep** | The Visualizer issues step *k* only after the Driver's result for step *k−1* has arrived. |
| **Ego vehicle** | The recorded KITTI-360 vehicle. Its pose comes from the dataset (open loop). |
| **Shadow vehicle** | The Driver's simulated vehicle, driven by the vehicle model's control output. It is visualised as a ghost next to the ego vehicle. |
| **Sequence pack** | The preprocessed, random-access KITTI-360 sequence format produced by the packer tool. |
| **Oracle** | The ground-truth perception module that derives "detections" from KITTI-360 3D box annotations. |
| **World state** | The Driver's model of what it knows: vehicles, sensors, perceived objects and the vehicle model's belief. Its changes are streamed to the Visualizer. |
| **Scene stream** | The pub/sub messages that carry world-state changes and sensor state from the Driver to the Visualizer, committed per step. |
| **Snapshot** | An immutable, render-relevant copy of the scene state produced by the simulation thread for the renderer's frame chain. |
| **Virtual resource** | A render-graph resource handle that is bound to a physical GPU resource only at execution time. |
| **View** | One camera rendering through the render graph, for example the main viewport or a picture-in-picture view. |

## 6. Third-party dependencies

| Library | Use | Location / acquisition | Status |
|---|---|---|---|
| Qt 6.11.1 (Core, Gui, Qml, Quick, ShaderTools, GuiPrivate) | Visualizer UI, QRhi | System install, `find_package` | Existing |
| DirectX 12 (Windows SDK) | RHI | Windows SDK ≥ 10.0.26100 | Existing |
| DXC | Offline shader compilation | `tools/dxc` | Existing |
| D3D12MemoryAllocator 3.2.0 | GPU memory allocation | `external/` | Existing |
| EnTT 4.0.0 | ECS | `external/` | Existing |
| Eigen 5.0.1 | Math | `external/` | Existing |
| stdexec | Sender/receiver | `external/` | Existing |
| ASIO 1.36.0 | Async file IO | `external/` | Existing |
| Catch2 3.x | Unit tests | FetchContent | Existing |
| **zenoh-c + zenoh-cpp (1.x)** | Inter-process communication | `external/` (prebuilt zenoh-c release + header-only zenoh-cpp) | **New** |
| **Jolt Physics (5.x)** | Shadow vehicle physics and collision queries | `external/` | **New** |
| **yaml-cpp (0.8.x)** | Scenario and configuration files | `external/` | **New** |
| **fastgltf (+ simdjson)** | glTF 2.0 loading | `external/` | **New** |
| **spdlog** | Logging | `external/` | **New** |
| **xxHash (0.8.x)** | xxh3 hashes: shader manifest, sequence-pack verification, `--record-hashes` determinism files, asset deduplication | `external/` (header-only, `XXH_INLINE_ALL`) | **New** |
| H3 4.4.1 | Not used in iteration 1 (kept for future geo indexing) | `external/` | Existing |
| Windows Imaging Component | PNG/JPEG decoding (dataset images, glTF textures) | OS | Built in |

New dependencies are vendored in `external/` with pinned versions, as the existing ones are. The choices are discussed in [99](99-decisions-and-open-questions.md).
