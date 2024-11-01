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
		Eigen3::Eigen
	PRIVATE
		xxHash::xxhash
)

target_link_libraries(AVIS_LIB
	PUBLIC
		Eigen3::Eigen
	PRIVATE
		xxHash::xxhash
)

target_sources(AVIS_LIB
	PRIVATE
		configuration.cpp
		configuration/configuration_parser.cpp
		configuration/ini_parser.cpp
		data/page_controller.cpp
		data/page_view.cpp
		data/point_cloud.cpp
		data_formats/json/json_parser.cpp
		data_formats/json/json_tokenizer.cpp
		data_formats/ply/ply_parser.cpp
		geometry/data_layout.cpp
		geometry/data_store.cpp
		input/details/input_wrapper.cpp
		input/input_device_controller_dualsense.cpp
		input/input_device_keyboard.cpp
		input/input_device_mouse.cpp
		io/file_descriptor.cpp
		io/io_context.cpp
		io/io_request.cpp
		io/io_service.cpp
		io/operation_context.cpp
		parallel/thread_pool.cpp
		platform/basic_window.cpp
		platform/basic_window_messages.cpp
		runtime.cpp
		step_timer.cpp
		streams/memory_stream.cpp
		utils/unique_handle.cpp
		window.cpp
)

# Build HLSL shaders
enable_build_shaders(AVIS_LIB)

set(VERTEX_SHADERS
	${AVIS_ASSETS_DIR}/shaders/triangle_vs.hlsl)
build_shaders(${VERTEX_SHADERS} vs_6_5 ${AVIS_ASSETS_DIR}/cso)

set(PIXEL_SHADERS
	${AVIS_ASSETS_DIR}/shaders/triangle_ps.hlsl)
build_shaders(${PIXEL_SHADERS} ps_6_5 ${AVIS_ASSETS_DIR}/cso)

# set(COMPUTE_SHADERS
# 	${AVIS_ASSETS_DIR}/shaders/triangle_vs.hlsl
# 	${AVIS_ASSETS_DIR}/shaders/triangle_ps.hlsl)
# build_shaders(${COMPUTE_SHADERS} cs_6_5 ${AVIS_ASSETS_DIR}/cso)