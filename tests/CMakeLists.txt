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

target_sources(AVIS_TESTS
	PRIVATE
		helpers/allocation_failure.cpp
		test_asio_senders_receivers.cpp
		test_error.cpp
		test_external_dependencies.cpp
		test_finite_state_machine.cpp
		test_helpers.cpp
		test_io_context.cpp
		test_log.cpp
		test_observables.cpp
		test_parallel_backend.cpp
		test_runtime.cpp
		test_runtime_configuration.cpp
		test_thread_pool.cpp)

include(CTest)
add_test(NAME "Avis Tests" COMMAND AVIS_TESTS)