add_executable(AVIS_VISUALIZER WIN32)

target_compile_features(AVIS_VISUALIZER
	PUBLIC cxx_std_${AVIS_REQUIRED_CXX_STANDARD})

set_target_properties(AVIS_VISUALIZER
	PROPERTIES
		OUTPUT_NAME "visualizer"

		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)

target_compile_definitions(AVIS_VISUALIZER
	PUBLIC
		AVIS_LIB
)

target_include_directories(AVIS_VISUALIZER
	PRIVATE
		AVIS_LIB
)

target_link_libraries(AVIS_VISUALIZER
	PRIVATE
		AVIS_LIB
)

target_sources(AVIS_VISUALIZER
	PRIVATE
		visualizer.cpp
)

# Copy the assets to the output directory
add_custom_command(
	TARGET AVIS_VISUALIZER POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${PROJECT_SOURCE_DIR}/assets
		$<TARGET_FILE_DIR:AVIS_VISUALIZER>/assets
	COMMENT "Copy assets to the folder \"$<TARGET_FILE_DIR:AVIS_VISUALIZER>/assets\""
)