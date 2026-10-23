# Task 0 — Integrate the vendored external libraries

| | |
|---|---|
| Design | [00 §6](../design/00-overview.md#6-third-party-dependencies), [13 §2.1](../design/13-build-and-quality.md#21-library-targets), [13 §4](../design/13-build-and-quality.md#4-dependency-management), assumption A-63 ([99](../design/99-decisions-and-open-questions.md)) |
| Files | `external/CMakeLists.txt`, `external/README.md` (new), `lib/avis/CMakeLists.txt` |
| Libraries | spdlog 1.17.0, yaml-cpp 0.9.0, fastgltf 0.9.1, simdjson 4.6.11 (all already in `external/`) |
| Needed by | [Task 1](01-foundations.md) (spdlog), [Task 4](04-library-runtime.md) (yaml-cpp, for the configuration mapping); fastgltf is needed later by scene asset loading ([08 §5](../design/08-scene-management.md#5-assets-gltf)) |

The four libraries are already in `external/` but aren't built or linked. This task wires them in once, so later tasks just use them. It changes no Avis behaviour.

> **Status (2026-10-18): done.** MSVC 14.51 (Visual Studio 18), CMake 4.3.3, Ninja, presets `windows-debug` and `windows-release`.

## 0.1 simdjson (for fastgltf)
fastgltf 0.9.1's `cmake/dependencies.cmake` downloads simdjson from GitHub at configure time unless a `simdjson::simdjson` target already exists ([13 §4](../design/13-build-and-quality.md#4-dependency-management)). The full simdjson **4.6.11** source tree is vendored in `external/simdjson-4.6.11/` (the version fastgltf pins as `SIMDJSON_TARGET_VERSION`). Its own CMake project defines `simdjson::simdjson`, so it only has to be added before fastgltf.

- [x] In `external/CMakeLists.txt`, before fastgltf:

  ```cmake
  # Set simdjson build options; must come before fastgltf so it doesn't download its own copy
  set(SIMDJSON_DEVELOPER_MODE OFF)
  set(SIMDJSON_INSTALL OFF)
  set(SIMDJSON_BUILD_STATIC_LIB OFF)
  add_subdirectory(simdjson-4.6.11 EXCLUDE_FROM_ALL)
  ```

  As a subproject, simdjson builds only its library (no tests, benchmarks or single-header generation). It's static because `BUILD_SHARED_LIBS` is unset; keep it that way, or the target becomes a DLL.
- [x] Keep simdjson's exception interface (`SIMDJSON_EXCEPTIONS`, default `ON`). The build uses `/EHsc` ([13 §1](../design/13-build-and-quality.md#1-toolchain)).
- [x] Check that fastgltf uses the vendored simdjson: after a configure, the log doesn't show fastgltf's `Successfully downloaded` message for `simdjson.h` / `simdjson.cpp`, and `external/fastgltf-0.9.1/deps/` doesn't exist. A fully offline configure isn't possible yet (see 0.6), so this is the check that matters for this task.

## 0.2 `external/CMakeLists.txt`
Follow the existing style: set options right before each `add_subdirectory(… EXCLUDE_FROM_ALL)`. Use `set(... CACHE BOOL "" FORCE)` or plain `set` (CMake ≥ 3.13 policy `CMP0077` lets normal variables override `option()`; set `cmake_policy(SET CMP0077 NEW)` if a library's `cmake_minimum_required` is older).

- [x] spdlog:

  ```cmake
  set(SPDLOG_USE_STD_FORMAT ON)
  set(SPDLOG_BUILD_SHARED OFF)
  set(SPDLOG_BUILD_EXAMPLE OFF)
  set(SPDLOG_INSTALL OFF)
  add_subdirectory(spdlog-1.17.0 EXCLUDE_FROM_ALL)
  ```

- [x] yaml-cpp:

  ```cmake
  set(YAML_CPP_BUILD_TOOLS OFF)
  set(YAML_CPP_BUILD_CONTRIB OFF)
  set(YAML_CPP_BUILD_TESTS OFF)
  set(YAML_CPP_INSTALL OFF)
  set(YAML_CPP_FORMAT_SOURCE OFF)
  set(YAML_BUILD_SHARED_LIBS OFF)
  add_subdirectory(yaml-cpp-0.9.0 EXCLUDE_FROM_ALL)
  ```

- [x] fastgltf (after simdjson):

  ```cmake
  set(FASTGLTF_COMPILE_AS_CPP20 ON)
  set(FASTGLTF_ENABLE_INSTALL OFF)
  add_subdirectory(fastgltf-0.9.1 EXCLUDE_FROM_ALL)
  ```

- [x] Make sure third-party warnings don't fail the build: they are compiled without `/WX` ([13 §1](../design/13-build-and-quality.md#1-toolchain)). If `AvisWarnings.cmake` sets flags globally, move them onto Avis targets only.

## 0.3 Link from `AVIS_LIB`
Following [13 §2.1](../design/13-build-and-quality.md#21-library-targets):

- [x] `target_link_libraries(AVIS_LIB PUBLIC spdlog::spdlog)`: public, because `include/avis/log.h` exposes `spdlog::logger`.
- [x] `target_link_libraries(AVIS_LIB PRIVATE yaml-cpp::yaml-cpp fastgltf::fastgltf)`: no public Avis header includes them.
- [x] While there, tidy the existing `target_include_directories(AVIS_LIB PUBLIC … ASIO Eigen3::Eigen …)` block: it passes target names as include directories, which has no effect. Linking the targets already brings their include paths.
- Applications that parse their own YAML configuration (`visualizer.yaml`, `driver.yaml`) link `yaml-cpp::yaml-cpp` themselves. Nothing to do yet: no application reads YAML today.

## 0.4 `external/README.md`
- [x] Create it ([13 §4](../design/13-build-and-quality.md#4-dependency-management)) with one row per vendored library: name, version, licence, upstream URL, and the options set in `external/CMakeLists.txt`. Include the existing libraries (asio, D3D12MemoryAllocator, Eigen, EnTT, H3, stdexec with its commit hash) as well as spdlog, yaml-cpp, fastgltf and simdjson.

## 0.5 Smoke tests
- [x] `tests/test_external_dependencies.cpp` with one test case per library, so a broken integration fails in CI and not in the first feature that uses it:
  - spdlog: log to a `ringbuffer_sink_mt` with a `std::format`-style argument, check the formatted line;
  - yaml-cpp: `YAML::Load("runtime: { io_threads: 1 }")` and read the value back;
  - fastgltf: parse a minimal embedded glTF JSON (`{"asset":{"version":"2.0"}}`) with `fastgltf::Parser` and `GltfDataBuffer`, expect `Error::None`.
- [x] Because yaml-cpp and fastgltf are `PRIVATE` to `AVIS_LIB`, link them to `AVIS_TESTS` explicitly for this test.

## 0.6 No new configure-time downloads
A fresh configure still needs the network, and not because of this task: `external/stdexec`'s CMake downloads `RAPIDS.cmake`, `execution.bs`, rapids-cmake, CPM and icm, and `tests/CMakeLists.txt` fetches Catch2 v3.4.0 with `FetchContent`. What this task guarantees is that **the four new libraries add no downloads**.

- [x] Configure once with the network available (`cmake --preset windows-debug`), so the existing downloads land in `out/build/windows-debug/_deps` and the stdexec build folder.
- [x] Re-configure the same build folder **disconnected**:

  ```bash
  cmake --preset windows-debug -DFETCHCONTENT_FULLY_DISCONNECTED=ON
  ```

  It must succeed. stdexec skips its `file(DOWNLOAD …)` calls because the files already exist in its build folder, and `FETCHCONTENT_FULLY_DISCONNECTED` stops FetchContent/CPM from contacting the network. Any failure now comes from spdlog, yaml-cpp, fastgltf or simdjson.
- [x] Check that `git status external/` shows no new or modified files after configuring and building, so nothing writes into the vendored folders.

## Acceptance criteria
- [x] 0.1 and 0.6 hold: fastgltf uses the vendored simdjson, a disconnected re-configure of an already-populated build folder succeeds, and nothing is written into `external/`.
- [x] `AVIS_LIB`, the Visualizer and `avis_tests` build in Debug and Release.
- [x] The smoke tests and the full suite pass in **Debug** (13 test cases, 28 assertions).
- [x] The tests pass in **Release**. At first Windows Application Control blocked the unsigned Release `avis_tests.exe` on the development machine; later Release builds ran, and the full suite passed (checked during Task 3).
- [x] [00 §6](../design/00-overview.md#6-third-party-dependencies): spdlog, yaml-cpp, fastgltf and simdjson are marked *Existing*.

## Open points
- **Fully offline fresh configure.** This needs (a) Catch2 vendored as `external/Catch2-<version>/` instead of `FetchContent`, and (b) stdexec added without its own `CMakeLists.txt`, which downloads rapids-cmake: for example an `INTERFACE` target `STDEXEC::stdexec` over `external/stdexec/include` plus a checked-in `stdexec_version_config.hpp`. That's outside this task; [13 §4](../design/13-build-and-quality.md#4-dependency-management) records the current state.
