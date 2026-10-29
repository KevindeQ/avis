FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.4.0
)

FetchContent_MakeAvailable(Catch2)

add_executable(AVIS_TESTS)

target_compile_features(AVIS_TESTS
	PUBLIC cxx_std_${AVIS_REQUIRED_CXX_STANDARD})

set_target_properties(AVIS_TESTS
	PROPERTIES
		OUTPUT_NAME "avis_tests"

		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)

target_link_libraries(AVIS_TESTS
	PRIVATE
		AVIS_LIB

		# Private dependencies of AVIS_LIB, linked directly for test_external_dependencies.cpp
		fastgltf::fastgltf
		yaml-cpp::yaml-cpp

		Catch2::Catch2WithMain)

# White-box tests include private library headers (lib/avis/...), e.g. the IO operation registry
target_include_directories(AVIS_TESTS
	PRIVATE
		${CMAKE_SOURCE_DIR}/lib)

target_sources(AVIS_TESTS
	PRIVATE
		helpers/allocation_failure.cpp
		helpers/rhi_fixture.cpp
		test_asio_senders_receivers.cpp
		test_async_file.cpp
		test_check.cpp
		test_enum_flags.cpp
		test_error.cpp
		test_external_dependencies.cpp
		test_file_operations.cpp
		test_finite_state_machine.cpp
		test_helpers.cpp
		test_io_context.cpp
		test_io_error.cpp
		test_io_public_headers.cpp
		test_log.cpp
		test_mapped_file.cpp
		test_observables.cpp
		test_operation_registry.cpp
		test_parallel_backend.cpp
		test_rhi_command_lists.cpp
		test_rhi_completions.cpp
		test_rhi_conversions.cpp
		test_rhi_descriptor_heaps.cpp
		test_rhi_device.cpp
		test_rhi_leaf_mutex.cpp
		test_rhi_lifetime.cpp
		test_rhi_pipelines.cpp
		test_rhi_public_headers.cpp
		test_rhi_resources.cpp
		test_rhi_sharing.cpp
		test_rhi_slot_map.cpp
		test_rhi_transient.cpp
		test_rhi_types.cpp
		test_runtime.cpp
		test_runtime_configuration.cpp
		test_sender_of.cpp
		test_thread_pool.cpp)

# RHI test shaders (docs/tasks/06-rhi.md §6.8): compiled at build time into headers with the bytecode as byte arrays,
# so the tests need no shader library. Generated, never checked in, rebuilt when the HLSL changes.
set(AVIS_TEST_DXC "${CMAKE_SOURCE_DIR}/tools/dxc/bin/x64/dxc.exe")
if(NOT EXISTS "${AVIS_TEST_DXC}")
	message(FATAL_ERROR "The RHI tests compile their shaders with DXC, expected at ${AVIS_TEST_DXC}")
endif()
set(AVIS_TEST_SHADER_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders")

function(avis_test_shader source entry profile name)
	set(input "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}")
	set(output "${AVIS_TEST_SHADER_DIRECTORY}/${name}.h")
	add_custom_command(
		OUTPUT "${output}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${AVIS_TEST_SHADER_DIRECTORY}"
		COMMAND "${AVIS_TEST_DXC}" -T ${profile} -E ${entry} -HV 2021 -WX -Fh "${output}" -Vn ${name} "${input}"
		DEPENDS "${input}"
		COMMENT "Compiling test shader ${name}"
		VERBATIM)
	target_sources(AVIS_TESTS PRIVATE "${output}")
endfunction()

avis_test_shader(rhi_test_triangle.hlsl vertex_main vs_6_6 rhi_test_vertex_shader)
avis_test_shader(rhi_test_triangle.hlsl pixel_main ps_6_6 rhi_test_pixel_shader)
avis_test_shader(rhi_test_fill.hlsl fill_main cs_6_6 rhi_test_compute_shader)

target_include_directories(AVIS_TESTS
	PRIVATE
		"${AVIS_TEST_SHADER_DIRECTORY}")

include(CTest)
add_test(NAME "Avis Tests" COMMAND AVIS_TESTS)