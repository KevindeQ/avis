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
		ASIO
		Eigen3::Eigen
	PRIVATE
		xxHash::xxhash
)

target_link_libraries(AVIS_LIB
	PUBLIC
		ASIO
		Eigen3::Eigen
	PRIVATE
		ryml::ryml
		xxHash::xxhash
)

target_sources(AVIS_LIB
	PRIVATE
		avis/configuration.cpp
		avis/data/page_controller.cpp
		avis/data/page_view.cpp
		avis/data/point_cloud.cpp
		avis/data_formats/json/json_parser.cpp
		avis/data_formats/json/json_tokenizer.cpp
		avis/data_formats/ply/ply_parser.cpp
		avis/geometry/data_layout.cpp
		avis/geometry/data_store.cpp
		avis/graphics/render_device.cpp
		avis/graphics/root_signature.cpp
		avis/input/details/input_wrapper.cpp
		avis/input/input_device_controller_dualsense.cpp
		avis/input/input_device_keyboard.cpp
		avis/input/input_device_mouse.cpp
		avis/io/io_service.cpp
		avis/parallel/thread_pool.cpp
		avis/platform/basic_window.cpp
		avis/platform/basic_window_messages.cpp
		avis/render_graph/render_pass.cpp
		avis/render_graph/render_pass_builder.cpp
		avis/render_graph/render_pipeline.cpp
		avis/runtime.cpp
		avis/step_timer.cpp
		avis/streams/memory_stream.cpp
		avis/utils/unique_handle.cpp
		avis/window.cpp
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