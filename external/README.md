# External libraries

Vendored third-party sources, pinned to the versions below ([13 §4](../docs/design/13-build-and-quality.md#4-dependency-management)). Each library is added in [`CMakeLists.txt`](CMakeLists.txt) with `add_subdirectory(… EXCLUDE_FROM_ALL)`; its build options are set right before that call.

| Library | Version | Folder | Licence | Upstream | CMake target | Options set in `CMakeLists.txt` |
|---|---|---|---|---|---|---|
| ASIO (standalone) | 1.36.0 | `asio-1.36.0` | Boost Software License 1.0 | https://think-async.com/Asio/ | `ASIO` (`INTERFACE`, `ASIO_STANDALONE`, `ASIO_NO_DEPRECATED`) | — |
| D3D12 Memory Allocator | 3.2.0 | `D3D12MemoryAllocator-3.2.0` | MIT | https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator | `D3D12MemoryAllocator` | — |
| Eigen | 5.0.1 | `eigen-5.0.1` | MPL 2.0 (see `COPYING.README`) | https://gitlab.com/libeigen/eigen | `Eigen3::Eigen` | `EIGEN_BUILD_TESTING`, `EIGEN_LEAVE_TEST_IN_ALL_TARGET`, `EIGEN_BUILD_BTL`, `EIGEN_BUILD_SPBENCH`, `EIGEN_BUILD_DOC`, `EIGEN_BUILD_DEMOS`, `EIGEN_SPLIT_LARGE_TESTS` all `OFF` |
| EnTT | 4.0.0 | `entt-4.0.0` | MIT | https://github.com/skypjack/entt | `EnTT::EnTT` | `ENTT_INCLUDE_NATVIS`, `ENTT_STANDARD_CPP` `ON`; `ENTT_USE_LIBCPP`, `ENTT_USE_SANITIZER`, `ENTT_USE_CLANG_TIDY`, `ENTT_INCLUDE_HEADERS`, `ENTT_INSTALL`, `ENTT_BUILD_TESTING`, `ENTT_BUILD_TESTBED` `OFF` |
| H3 | 4.4.1 | `h3-4.4.1` | Apache 2.0 | https://github.com/uber/h3 | `h3` | `ENABLE_COVERAGE`, `BUILD_BENCHMARKS`, `ENABLE_COUNTRY_BENCHMARKS`, `BUILD_FUZZERS`, `BUILD_FILTERS`, `BUILD_GENERATORS`, `ENABLE_LIBFUZZER` all `OFF` |
| stdexec | commit `61fb73d7` (2026-06-02) | `stdexec` | Apache 2.0 with LLVM exceptions | https://github.com/NVIDIA/stdexec | `STDEXEC::stdexec` | — (its CMake downloads rapids-cmake, CPM and icm at configure time, [13 §4](../docs/design/13-build-and-quality.md#4-dependency-management)) |
| simdjson | 4.6.11 | `simdjson-4.6.11` | Apache 2.0 or MIT | https://github.com/simdjson/simdjson | `simdjson::simdjson` (static) | `SIMDJSON_DEVELOPER_MODE`, `SIMDJSON_INSTALL`, `SIMDJSON_BUILD_STATIC_LIB` `OFF`. Added before fastgltf so fastgltf doesn't download its own copy. |
| fastgltf | 0.9.1 | `fastgltf-0.9.1` | MIT | https://github.com/spnda/fastgltf | `fastgltf::fastgltf` (static) | `FASTGLTF_COMPILE_AS_CPP20` `ON`; `FASTGLTF_ENABLE_INSTALL` `OFF` |
| spdlog | 1.17.0 | `spdlog-1.17.0` | MIT | https://github.com/gabime/spdlog | `spdlog::spdlog` (static) | `SPDLOG_USE_STD_FORMAT` `ON`; `SPDLOG_BUILD_SHARED`, `SPDLOG_BUILD_EXAMPLE`, `SPDLOG_INSTALL` `OFF` |
| yaml-cpp | 0.9.0 | `yaml-cpp-0.9.0` | MIT | https://github.com/jbeder/yaml-cpp | `yaml-cpp::yaml-cpp` (static) | `YAML_CPP_BUILD_TOOLS`, `YAML_CPP_BUILD_CONTRIB`, `YAML_CPP_BUILD_TESTS`, `YAML_CPP_INSTALL`, `YAML_CPP_FORMAT_SOURCE`, `YAML_BUILD_SHARED_LIBS` `OFF` |
| xxHash | 0.8.4 | `xxHash-0.8.4` (CMake project in `build/cmake`) | BSD 2-Clause (`LICENSE`; the unused `xxhsum` CLI is GPLv2) | https://github.com/Cyan4973/xxHash | `xxHash::xxhash` (static) | `XXHASH_BUILD_XXHSUM` `OFF`; `BUILD_SHARED_LIBS` `OFF` |

Not vendored here:

| Library | Version | Acquisition |
|---|---|---|
| Catch2 | 3.4.0 | `FetchContent` in [`tests/CMakeLists.txt`](../tests/CMakeLists.txt) |
| Qt | 6.11.1 | System install, `find_package(Qt6)` (`Qt6_ROOT` in `CMakePresets.json`) |

## Adding or updating a library
1. Put the sources in `external/<name>-<version>/`. Don't modify them; set options from this folder's `CMakeLists.txt` instead.
2. Add it to `CMakeLists.txt` with `add_subdirectory(… EXCLUDE_FROM_ALL)`, options first.
3. Add or update its row above.
4. Check that configuring doesn't download anything new and doesn't write into `external/` (`git status external/`).
