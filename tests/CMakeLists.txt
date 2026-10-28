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
		test_rhi_completions.cpp
		test_rhi_conversions.cpp
		test_rhi_descriptor_heaps.cpp
		test_rhi_device.cpp
		test_rhi_leaf_mutex.cpp
		test_rhi_lifetime.cpp
		test_rhi_public_headers.cpp
		test_rhi_resources.cpp
		test_rhi_slot_map.cpp
		test_rhi_types.cpp
		test_runtime.cpp
		test_runtime_configuration.cpp
		test_sender_of.cpp
		test_thread_pool.cpp)

include(CTest)
add_test(NAME "Avis Tests" COMMAND AVIS_TESTS)