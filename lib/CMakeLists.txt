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

		$<$<STREQUAL:$<TARGET_PROPERTY:AVIS_LIB,TYPE>,SHARED_LIBRARY>:ENGINE_BUILD_SHARED>
		$<$<STREQUAL:$<TARGET_PROPERTY:AVIS_LIB,TYPE>,SHARED_LIBRARY>:ENGINE_BUILD_STATIC>
		$<IF:$<CONFIG:Debug>,ENGINE_BUILD_DEBUG,ENGINE_BUILD_RELEASE>
)

target_include_directories(AVIS_LIB
	PUBLIC
		${AVIS_INCLUDE_DIR}
)

target_sources(AVIS_LIB
	PRIVATE
		core/streams/binary_stream.cpp
		core/streams/memory_stream.cpp
		core/streams/text_stream.cpp
		core/unique_handle.cpp

		middleware/configuration/configuration_parser.cpp
		middleware/configuration/ini_parser.cpp
		middleware/data_formats/json/json_parser.cpp
		middleware/data_formats/json/json_tokenizer.cpp
		middleware/data_formats/ply/ply_parser.cpp
		middleware/geometry/data_layout.cpp
		middleware/geometry/data_store.cpp
		middleware/input/input_context.cpp
		middleware/input/input_dispatcher.cpp
		middleware/platform/basic_window.cpp
		middleware/platform/basic_window_messages.cpp
		middleware/configuration.cpp
		middleware/runtime.cpp
		middleware/window.cpp

		runtime/graphics/command_context.cpp
		runtime/graphics/command_queue.cpp
		runtime/graphics/graphics_device.cpp
		runtime/graphics/graphics_system.cpp
		runtime/graphics/render_pass.cpp
		runtime/graphics/render_pass_builder.cpp
		runtime/graphics/render_pipeline.cpp
		runtime/graphics/resource.cpp
		runtime/graphics/resource_graph.cpp
		runtime/graphics/root_descriptor.cpp
		runtime/io/file_descriptor.cpp
		runtime/io/io_context.cpp
		runtime/io/io_request.cpp
		runtime/io/io_service.cpp
		runtime/io/operation_context.cpp
		runtime/parallel/thread_pool.cpp)