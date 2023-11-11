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
		OUTPUT_NAME "tests"

		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)

target_link_libraries(AVIS_TESTS
	PRIVATE
		Catch2::Catch2WithMain)

target_sources(AVIS_TESTS
	PRIVATE
		unit_tests.cpp
)

include(CTest)
add_test(NAME "Avis Tests" COMMAND AVIS_TESTS)