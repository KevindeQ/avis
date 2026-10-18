# 13 — Build, Tooling & Quality

## 1. Toolchain
| Item | Choice |
|---|---|
| OS | Windows 11 24H2+ x64 |
| Compiler | MSVC (Visual Studio 2022 17.12+ or newer), `/std:c++latest` until C++23 is fully exposed as `/std:c++23` |
| Build | CMake ≥ 3.30, Ninja Multi-Config generator (VS generator supported) |
| Windows SDK | ≥ 10.0.26100 |
| Shader compiler | DXC from `tools/dxc` (pinned) |
| Qt | 6.11.1 (MSVC 2022 x64 kit) |
| Python | ≥ 3.14 with `uv` |

Compiler flags: `/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /fp:precise /EHsc`. Warnings are errors in CI (`/WX`) for Avis code, not for `external/`. Release builds add `/O2 /GL` with `/LTCG`, and debug info (`/Zi`) is always generated.

## 2. CMake structure
```
CMakeLists.txt                  # project, options, Qt find, add_subdirectory(external, lib, apps, tests)
cmake/
  AvisShaders.cmake             # avis_add_shaders()
  WinDeployQT.cmake             # existing
  AvisWarnings.cmake
external/CMakeLists.txt         # vendored dependencies (EXCLUDE_FROM_ALL)
lib/avis/CMakeLists.txt         # AVIS_LIB: the single Avis library target (all library features)
apps/visualizer/CMakeLists.txt  # AVIS_VISUALIZER (Qt, AVIS_LIB)
apps/driver/CMakeLists.txt      # AVIS_DRIVER (AVIS_LIB, Jolt)
include/avis/driver_sdk/        # AVIS_DRIVER_SDK: header-only INTERFACE target for plug-ins
apps/driver/plugins/*/          # example plug-ins (SHARED libraries)
apps/stepper/CMakeLists.txt     # AVIS_STEPPER (headless lockstep CLI for tests, AVIS_LIB)
tests/CMakeLists.txt            # AVIS_TESTS (Catch2)
```

### 2.1 Library targets
All Avis library features build into **one CMake target, `AVIS_LIB`** (a static library defined in `lib/avis/CMakeLists.txt`). There are no per-feature sub-targets or object libraries. Features are separated by **namespace and directory**, not by CMake target.

`AVIS_LIB` links: stdexec, asio, spdlog, zenoh-cpp/zenoh-c, EnTT, Eigen, fastgltf, yaml-cpp, xxHash, D3D12MemoryAllocator, `d3d12.lib`, `dxgi.lib`, `dxguid.lib`. Public include directory: `include/`.

| Feature | Namespace | Location | Document |
|---|---|---|---|
| Error type, logging, enum flags, virtual clock, FSM | `avis`, `avis::log` | `include/avis/*.h` | [01 §6](01-system-architecture.md#6-error-handling-policy), [03](03-simulation-core.md) |
| Library runtime | `avis` | `include/avis/runtime.h` | [14](14-library-runtime.md) |
| Thread pool, task system (separate classes) | `avis::threading` | `include/avis/threading/` | [06](06-thread-pool.md), [15](15-task-system.md) |
| Async IO | `avis::io` | `include/avis/io/` | [07](07-io-layer.md) |
| Networking (Zenoh wrapper, framing, protocol messages) | `avis::network`, `avis::network::protocol` | `include/avis/network/` | [02](02-communication.md) |
| Simulation time, clock state, scenario model | `avis::simulation` | `include/avis/simulation/` | [03](03-simulation-core.md), [04](04-scenario-and-dataset.md) |
| Sequence-pack readers (used by the Driver only) | `avis::dataset` | `include/avis/dataset/` | [04 §4](04-scenario-and-dataset.md#4-sequence-pack-format) |
| Scene, components, scene graph, assets, snapshots | `avis::scene`, `avis::scene::components` | `include/avis/scene/` | [08](08-scene-management.md) |
| RHI (public API; D3D12 implementation private) | `avis::rhi` (`avis::rhi::d3d12` internal) | `include/avis/rhi/`, `lib/avis/rhi/d3d12/` | [09](09-rhi.md) |
| Render graph | `avis::graphics` | `include/avis/graphics/` | [10](10-render-graph.md) |

Sources mirror the headers under `lib/avis/<feature>/`.

### 2.1.1 Other targets
| Target | Kind | Namespace | Links | Document |
|---|---|---|---|---|
| `AVIS_VISUALIZER` | executable | `avis::visualizer` | `AVIS_LIB`, Qt | [12](12-visualizer.md), [11](11-renderer.md) |
| `AVIS_DRIVER` | executable | `avis::driver` | `AVIS_LIB`, Jolt, `AVIS_DRIVER_SDK` | [05](05-driver.md) |
| `AVIS_STEPPER` | executable (headless test CLI) | `avis::stepper` | `AVIS_LIB` | §6 |
| `AVIS_DRIVER_SDK` | header-only `INTERFACE` | C ABI (`avis_` prefix), `avis::sdk` (C++ wrapper) | — | [05 §5](05-driver.md#5-plug-in-sdk) |
| `AVIS_PLUGIN_EXAMPLE_*` | `SHARED` | plug-in specific | `AVIS_DRIVER_SDK` only | [05 §5.7](05-driver.md#57-example-plug-ins) |
| `AVIS_TESTS` | executable | test code | `AVIS_LIB`, Catch2 | §6 |

**Why the SDK isn't part of `AVIS_LIB`:** plug-ins only need the C header and must **not** link the Avis library (D-06: any compiler, any language). `AVIS_DRIVER_SDK` contains no compiled code. It just exposes `include/avis/driver_sdk/` as an include path, and it's the one CMake target outside `AVIS_LIB` that has headers under `include/avis/`.

### 2.2 Options
| Option | Default | Effect |
|---|---|---|
| `AVIS_BUILD_TESTS` | ON | Unit tests |
| `AVIS_BUILD_VISUALIZER` | ON | Requires Qt |
| `AVIS_BUILD_DRIVER` | ON | |
| `AVIS_BUILD_EXAMPLE_PLUGINS` | ON | |
| `AVIS_SHADER_DEBUG` | ON in Debug | `-Zi -Qembed_debug -Od` |
| `AVIS_ENABLE_ASAN` | OFF | `/fsanitize=address` |

## 3. Shader build pipeline
`avis_add_shaders(TARGET <target> SOURCES <files> OUTPUT_DIRECTORY <directory> [DEFINES …] [INCLUDE_DIRECTORIES …])`. Shaders are application data: the Visualizer calls it with `OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/data/shaders/visualizer` and `INCLUDE_DIRECTORIES data/shaders/visualizer/include` ([11 §13](11-renderer.md#13-shaders-location-and-build)). The Avis library contains no shaders.
- Each `.hlsl` file declares its entry points in a header comment, parsed by the CMake function:
  ```hlsl
  // @avis-shader entry=main stage=cs
  // @avis-shader entry=vertex_main stage=vs  entry=pixel_main stage=ps  permutations=ALPHA_MASK
  ```
- DXC command per entry and permutation:
  `dxc -T <stage>_6_6 -E <entry> -HV 2021 -enable-16bit-types -WX -O3 -I data/shaders/visualizer/include [-D…] -Fo <directory>/<name>.<entry>[.<permutation>].<stage>.cso -Fd <directory>/pdb/ -Qstrip_reflect`
  In debug: `-Zi -Qembed_debug -Od` instead of `-O3`.
- Dependency tracking via `-MF` depfiles (Ninja `DEPFILE`), so header changes recompile the dependent shaders.
- A final custom command (a `cmake -P` script) writes `<directory>/manifest.yaml` (id, stage, entry, permutation defines, xxh3 hash). YAML rather than JSON, because yaml-cpp is already a dependency. The shader library reads it at runtime ([09 §9.1](09-rhi.md#91-shader-library)).
- The Qt-side shaders (`data/shaders/visualizer/qt/*.vert|frag`) keep using `qt_add_shaders`. Qt compiles them to `.qsb`.
- Deployment: the compiled `data/shaders/visualizer/` directory (from the build output) is copied next to the executable by the install/deploy step, together with the other runtime data (`data/assets/`).

## 4. Dependency management
- Vendored sources live in `external/<name>-<version>/` with pinned versions. Each gets an entry in `external/README.md` with its version, licence and upstream URL.
- **zenoh-c** needs Rust to build from source. Iteration 1 vendors the **prebuilt release** of zenoh-c (DLL + import lib + headers) plus header-only **zenoh-cpp**. The DLL is copied next to the executables by a post-build step.
- **Jolt** is built from source with `CROSS_PLATFORM_DETERMINISTIC=ON`, `DOUBLE_PRECISION=OFF`, and the floating-point exception checks turned off in release. Single precision works because the Driver hands Jolt only coordinates relative to the run origin ([05 §3.3](05-driver.md#33-shadow-vehicle)).
- **xxHash** is vendored header-only and used for every xxh3 hash on the C++ side, so C++ and the Python tools (`xxhash` package) produce identical values. The shader-manifest script (`cmake -P`) can't call a C library, so the manifest hashes are computed by a tiny host tool built from the same header before the shaders.
- **CRC-32** (IEEE 802.3 polynomial, as `zlib.crc32` in Python) for the `.avs` record index is a small table-driven function in `AVIS_LIB` (`include/avis/dataset/crc32.h`). It needs no dependency.
- **fastgltf** brings simdjson. That JSON parser is internal to glTF loading (glTF is a JSON format) and isn't used for any Avis file format: Avis's own files are YAML or binary.
- Qt is deployed with the existing `WinDeployQT.cmake`.
- Python tools use `uv` lockfiles (`uv.lock`), with one project per tool under `tools/`.

## 5. Python tools
| Tool | Path | Purpose |
|---|---|---|
| `kitti360-pack` | `tools/kitti360-pack` | Raw KITTI-360 → sequence pack ([04 §5](04-scenario-and-dataset.md#5-packer-tool-toolskitti360-pack)) |
| `avis-pack-inspect` | `tools/kitti360-pack` (second entry point) | Print the manifest, verify hashes, dump records, export a frame to PNG/PLY for checking |
| `data-trace` | `tools/data-trace` (existing) | Data tracing utility |
| `avis-hashdiff` | `tools/avis-hashdiff` | Compare two `--record-hashes` outputs and report the first diverging step (determinism checks) |

Tools follow the existing `data-trace` layout (`pyproject.toml`, `src/<package>`, `tests/`, `uv_build` backend) and use `pytest`. They're not embedded in the applications (idea.md: no scripting role).

## 6. Testing strategy
| Level | Scope | Tooling |
|---|---|---|
| Unit | runtime, thread pool, task system, IO, FSM, virtual clock, scene graph, snapshot buffer, render-graph compiler (null RHI), serializer, scenario validation, sequence-pack readers | Catch2, `AVIS_TESTS` |
| GPU integration | RHI resource and barrier smoke tests, small render-graph executions with readback, GTAO/TAA shader unit tests on synthetic inputs | Catch2 tag `[gpu]`, skipped when the capability gate fails |
| Golden image | Render a fixed scenario frame (fixed camera, TAA off, fixed exposure) and compare to a reference PNG with a perceptual threshold (FLIP/PSNR). The golden images are produced with the renderer's own screenshot capture (11 §12.1), so that path is exercised too. | `[gpu][golden]`, references in `tests/data/golden/` |
| Screenshot | The viewport capture equals a direct readback of `ldr_color` byte for byte; a custom-resolution capture has the requested size, its 8-sample accumulation is stable across runs, and its mean luminance matches the viewport capture within a tolerance; the PNG text chunks hold the expected metadata; requests over the size or VRAM limit are refused with a message | `[gpu]` |
| Protocol | Visualizer lockstep coordinator and scene stream ↔ a mock Driver in-process, over a Zenoh loopback session: scouting and `describe`, `connect` (including `busy`), retries with stream re-publication, out-of-order, stream messages arriving before the reply or missing (`stream_incomplete`), liveliness loss. **Inactivity timeout:** a mock step that computes for 20 s while sending heartbeats doesn't pause (busy notice shown); a mock publish that trickles a 30 MB step over 10 s doesn't pause; a mock Driver that goes silent mid-step pauses with `unresponsive` after `communication_timeout`; a late result after that pause is committed by *Retry* without re-executing the step. | Catch2 |
| Determinism | Run a scenario headless twice (Driver + a headless "stepper" CLI that speaks the Visualizer protocol) and compare `--record-hashes` output | CTest script + `avis-hashdiff` |
| Plug-in fault | A test plug-in that throws, returns errors, dereferences null, returns malformed outputs, or sleeps, to confirm Driver fault reporting and Visualizer pause behaviour. After an SEH fault the module is reported in `pinned_plugins[]` and its file stays locked. | CTest |
| Plug-in workflow | (1) **Unload before rebuild:** with no run loaded, a plug-in DLL can be overwritten; after `load` it's locked; after `unload` it can be overwritten again; the next `load` runs the new build. (2) **Rescan:** a DLL added, changed or removed under the search path shows up in `rescan` and in the next `load` reply. (3) **Parameter schema:** valid parameters load with defaults merged; type, range and enum errors give `invalid_parameters` with the right paths; unknown keys give warnings; a malformed schema rejects the plug-in. (4) **Debug mode:** with `--debug`, a step that freezes the whole process (no heartbeats) for longer than `communication_timeout` and Zenoh's default lease completes without a pause or a disconnect. | CTest + stepper |
| Plug-in ABI | `avis_plugin.h` compiled as C11 with MSVC and clang-cl (the `_Static_assert` layout checks must pass), the pure-C example plug-in loaded and stepped by the Driver, `AVIS_PLUGIN_ABI_FINGERPRINT` identical under MSVC and clang-cl, and test plug-ins with a different `api_version` (major or minor) or a different fingerprint rejected at scan time with the expected `rejected_plugins[]` reason | CTest |
| Python | Packer on two synthetic mini-sequence fixtures, a **training** one (boxes and semantics) and a **test-split** one (sensor data and poses only), and a format round trip with the C++ reader (via a small C++ test executable). Checks: the reference timeline (`frames.avs`, per-stream timestamps relative to it, `step_count`, step 0 covering `(−Δt, 0]`); capabilities in the manifest; ground tiles (median per cell, hole filling up to 2 m, no-ground beyond, shared tile edges); `calibration.yaml` for pinhole and MEI cameras; and MEI `project` / `unproject` round trips against reference values computed with the KITTI-360 devkit formulas. | pytest + Catch2 |
| Dataset capabilities | Loading `ground_truth_oracle` or `jolt_wheeled` on the test-split fixture fails with the expected diagnostic; `kinematic_bicycle` loads with a warning and follows the recorded height | CTest + stepper |

A small **headless stepper** (`apps/stepper`, a CLI) implements the Visualizer side of the lockstep protocol without UI or rendering. It's used for determinism tests and CI.

## 7. Logging
- **spdlog**, with named loggers per component: `core`, `io`, `network`, `rhi`, `graph`, `render`, `scene`, `simulation`, `ui`, `driver`, `plugin.<name>`.
- Sinks: a rotating file (`logs/<application>-<date>.log`, 5 × 10 MiB), an MSVC debug output sink, a console sink (Driver), and a UI ring-buffer sink (Visualizer log panel).
- The Driver additionally forwards `info` and above to Zenoh (`…/log`).
- Format: `[2026-10-16 14:03:22.123] [rhi] [warn] [t:avis-worker-3] message`.
- Levels are configurable per logger in the app configuration.

## 8. Diagnostics
- Crash handling: `SetUnhandledExceptionFilter` writes a minidump to `logs/` (both apps).
- PIX: debug and release builds carry PIX event markers. A `--pix-capture` flag loads `WinPixGpuCapturer.dll` if present, for programmatic captures.
- A statistics overlay in the viewport shows CPU frame, GPU per-pass timings, the point budget in use, and memory (aliased and non-aliased).

## 9. Coding standards
- `.clang-format` (existing) is enforced by `clang-format-all.ps1` and a CI check.
- `clang-tidy` (from `tools/llvm`) runs with a curated profile: modernize, bugprone, performance, and readability-identifier-naming for `snake_case`.
- Public headers must not include `<d3d12.h>`, `<dxgi*.h>`, Qt or Zenoh headers (except for app code). A CI grep rule enforces this for `include/avis/` ([09 §1](09-rhi.md#1-goals-and-constraints)).
- `avis/threading/thread_pool.*` and `thread_handle.*` must not include task-system headers (`task_system.h`, `task_scheduler.h`, `work_stealing_deque.h`) or stdexec. A CI grep rule enforces this, which keeps the one-way dependency between the two classes ([06 §5](06-thread-pool.md#5-separation-from-the-task-system)).
- Every public API returning a failure path uses `avis::result<T>` or a sender with `set_error(avis::error)`.

### 9.1 One variable per declaration
Declare **one variable per declaration statement**. Never declare several variables on one line with a single type:

```cpp
// not allowed
std::uint32_t width = 1, height = 1;
float x, y, z;

// required
std::uint32_t width = 1;
std::uint32_t height = 1;
float x;
float y;
float z;
```

This applies to local variables, struct and class members (including one-line C structs in the plug-in SDK), and function-scope statics. Function parameter lists aren't declarations in this sense. clang-tidy's `readability-isolate-declaration` check enforces it.

### 9.2 No abbreviations in names
Names are written out in full words: types, functions, variables, members, enumerators, namespaces, files, configuration keys and CLI flags. For example `description` not `desc`, `configuration` not `config`, `context` not `ctx`, `command_list` not `cl`, `simulation` not `sim`, `network` not `net`, `parameters` not `params`, `maximum`/`minimum` not `max`/`min`, `previous`/`current` not `prev`/`curr`, `source`/`destination` not `src`/`dst`, `delta_time` not `dt`, `synchronization` not `sync`, `information` not `info`.

Exceptions apply only in these extreme cases:

| Exception | Examples | Rationale |
|---|---|---|
| Established acronyms of the problem domain | `gpu`, `cpu`, `io`, `rhi`, `sdk`, `id`, `uuid`, `ui`, `rpc`, `pso`, `srv`, `uav`, `rtv`, `dsv`, `ao`, `gtao`, `taa`, `csm`, `pbr`, `ibl`, `hdr`, `ldr`, `lod`, `aabb`, `fov`, `fps`, `gltf`, `yaml`, `hlsl`, `oxts`, `gnss`, `imu` | These are the names the domain uses. Spelling them out (`unordered_access_view`) hurts readability. |
| SI unit suffixes | `_ns`, `_ms`, `_hz`, `_px`, `_m` | Units are part of the value's meaning and are universally recognised |
| Names fixed by external APIs or formats | `com_ptr`, `sync_wait`, `D3D12_BARRIER_SYNC_*`, Qt's `adapterLuidLow`, KITTI-360 field names | Must match the external definition |
| Mathematical notation in formulas and pseudocode | `t_k`, `alpha`, loop indices `i`, `j`, `k`, components `x`, `y`, `z`, `w` | Conventional notation, not names in the API |
| Existing type names explicitly kept | `com_ptr` (existing class) | Retained by decision |

clang-tidy's `readability-identifier-naming` can't detect abbreviations, so this guideline is checked in code review. A list of banned name fragments (`desc`, `config`, `ctx`, `cfg`, `tmp`, `idx`, `cnt`, `num`, `prev`, `curr`, `src`, `dst`, …) is kept in `tools/naming-check/banned.txt` for an optional CI grep.

## 10. Continuous integration (suggested)
GitHub Actions on `windows-2025` runners:
1. Configure and build Debug and Release (Ninja).
2. Unit tests. GPU tests are skipped on hosted runners, because they lack a capable GPU (WARP may not pass the enhanced-barrier gate).
3. Format and clang-tidy checks.
4. `uv run pytest` for each tool.
5. A determinism test with the stepper on the synthetic mini-sequence.

A self-hosted GPU runner can be added later for `[gpu]` and golden-image tests.
