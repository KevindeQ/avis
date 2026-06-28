add_library(AVIS_LIB STATIC)

target_compile_features(AVIS_LIB
	PUBLIC cxx_std_${AVIS_REQUIRED_CXX_STANDARD})

set_target_properties(AVIS_LIB
	PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)

target_compile_definitions(AVIS_LIB
	PUBLIC
		UNICODE
		_UNICODE
		$<$<BOOL:${WIN32}>:ENGINE_PLATFORM_WINDOWS>
		$<$<STREQUAL:${CMAKE_CXX_COMPILER_ID},MSVC>:ENGINE_COMPILER_MSVC>

		$<IF:$<CONFIG:Debug>,ENGINE_BUILD_DEBUG,ENGINE_BUILD_RELEASE>
)

target_include_directories(AVIS_LIB
	PUBLIC
		${AVIS_INCLUDE_DIR}

		ASIO
		Eigen3::Eigen
		EnTT::EnTT
		h3
		STDEXEC::stdexec
)

target_link_libraries(AVIS_LIB
	PUBLIC
		ASIO
		Eigen3::Eigen
		EnTT::EnTT
		h3
		STDEXEC::stdexec
)

target_sources(AVIS_LIB
	PRIVATE
		virtual_clock_state.cpp

		scene/semantic_scene_graph.cpp
)
