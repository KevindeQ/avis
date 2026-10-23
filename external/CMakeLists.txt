add_subdirectory(asio-1.36.0 EXCLUDE_FROM_ALL)

add_subdirectory(D3D12MemoryAllocator-3.2.0 EXCLUDE_FROM_ALL)

# Set Eigen build options
set(EIGEN_BUILD_TESTING OFF)
set(EIGEN_LEAVE_TEST_IN_ALL_TARGET OFF)
set(EIGEN_BUILD_BTL OFF)
set(EIGEN_BUILD_SPBENCH OFF)
set(EIGEN_BUILD_DOC OFF)
set(EIGEN_BUILD_DEMOS OFF)
set(EIGEN_SPLIT_LARGE_TESTS OFF)
add_subdirectory(eigen-5.0.1 EXCLUDE_FROM_ALL)

# Set Entt build options
set(ENTT_USE_LIBCPP OFF)
set(ENTT_USE_SANITIZER OFF)
set(ENTT_USE_CLANG_TIDY OFF)
set(ENTT_INCLUDE_HEADERS OFF)
set(ENTT_INCLUDE_NATVIS ON)
set(ENTT_INSTALL OFF)
set(ENTT_BUILD_TESTING OFF)
set(ENTT_BUILD_TESTBED OFF)
set(ENTT_STANDARD_CPP ON)
add_subdirectory(entt-4.0.0 EXCLUDE_FROM_ALL)

# Set H3 build options
set(ENABLE_COVERAGE OFF)
set(BUILD_BENCHMARKS OFF)
set(ENABLE_COUNTRY_BENCHMARKS OFF)
set(BUILD_FUZZERS OFF)
set(BUILD_FILTERS OFF)
set(BUILD_GENERATORS OFF)
set(ENABLE_LIBFUZZER OFF)
add_subdirectory(h3-4.4.1 EXCLUDE_FROM_ALL)

# Set stdexec build options. AVIS_LIB defines the parallel scheduler backend itself, so stdexec's default
# backend library must stay off
set(STDEXEC_BUILD_PARALLEL_SCHEDULER OFF CACHE BOOL "" FORCE)
add_subdirectory(stdexec EXCLUDE_FROM_ALL)

# Set simdjson build options. Must come before fastgltf: it links the existing simdjson::simdjson
# target instead of downloading its own copy at configure time.
set(SIMDJSON_DEVELOPER_MODE OFF)
set(SIMDJSON_INSTALL OFF)
set(SIMDJSON_BUILD_STATIC_LIB OFF)
add_subdirectory(simdjson-4.6.11 EXCLUDE_FROM_ALL)

# Set fastgltf build options
set(FASTGLTF_COMPILE_AS_CPP20 ON)
set(FASTGLTF_ENABLE_INSTALL OFF)
add_subdirectory(fastgltf-0.9.1 EXCLUDE_FROM_ALL)

# Set spdlog build options
set(SPDLOG_USE_STD_FORMAT ON)
set(SPDLOG_BUILD_SHARED OFF)
set(SPDLOG_BUILD_EXAMPLE OFF)
set(SPDLOG_INSTALL OFF)
add_subdirectory(spdlog-1.17.0 EXCLUDE_FROM_ALL)

# Set yaml-cpp build options
set(YAML_CPP_BUILD_TOOLS OFF)
set(YAML_CPP_BUILD_CONTRIB OFF)
set(YAML_CPP_BUILD_TESTS OFF)
set(YAML_CPP_INSTALL OFF)
set(YAML_CPP_FORMAT_SOURCE OFF)
set(YAML_BUILD_SHARED_LIBS OFF)
add_subdirectory(yaml-cpp-0.9.0 EXCLUDE_FROM_ALL)