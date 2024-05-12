add_executable(AVIS_DATABASE)

target_compile_features(AVIS_DATABASE
	PUBLIC cxx_std_${AVIS_REQUIRED_CXX_STANDARD})

set_target_properties(AVIS_DATABASE
	PROPERTIES
		OUTPUT_NAME "adb"

		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)

target_compile_definitions(AVIS_DATABASE
	PUBLIC
		AVIS_LIB
)

target_include_directories(AVIS_DATABASE
	PRIVATE
		AVIS_LIB
)

target_link_libraries(AVIS_DATABASE
	PRIVATE
		AVIS_LIB
		ASIO
)

target_sources(AVIS_DATABASE
	PRIVATE
		adb.cpp
)