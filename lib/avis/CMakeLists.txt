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

		# Windows 10+ APIs (the minimum OS is Windows 11, D-05). Without it asio assumes Windows 7 (0x0601) and hides
		# newer APIs from <windows.h>, e.g. FileRenameInfoEx for POSIX-semantics renames.
		_WIN32_WINNT=0x0A00

		$<$<BOOL:${WIN32}>:ENGINE_PLATFORM_WINDOWS>
		$<$<STREQUAL:${CMAKE_CXX_COMPILER_ID},MSVC>:ENGINE_COMPILER_MSVC>

		$<IF:$<CONFIG:Debug>,ENGINE_BUILD_DEBUG,ENGINE_BUILD_RELEASE>
)

# Third-party include directories come with the linked targets below
target_include_directories(AVIS_LIB
	PUBLIC
		${AVIS_INCLUDE_DIR}
)

target_link_libraries(AVIS_LIB
	PUBLIC
		ASIO
		Eigen3::Eigen
		EnTT::EnTT
		h3
		STDEXEC::stdexec

		# Public because include/avis/log.h exposes spdlog::logger
		spdlog::spdlog

	PRIVATE
		# No public Avis header includes these
		fastgltf::fastgltf
		yaml-cpp::yaml-cpp
)

target_sources(AVIS_LIB
	PRIVATE
		error.cpp
		log.cpp
		runtime.cpp
		runtime_configuration.cpp
		virtual_clock_state.cpp

		io/async_file.cpp
		io/detail/file_sender.cpp
		io/detail/operation_registry.cpp
		io/error.cpp
		io/file_operations.cpp
		io/io_context.cpp
		io/mapped_file.cpp

		scene/semantic_scene_graph.cpp

		threading/parallel_backend.cpp
		threading/parallel_helpers.cpp
		threading/pool_thread.cpp
		threading/thread_handle.cpp
		threading/thread_pool.cpp
		threading/thread_priority.cpp
		threading/thread_state.cpp
)
