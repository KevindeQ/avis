FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.4.0
)

FetchContent_MakeAvailable(Catch2)

add_executable(AVIS_TESTS)

# The Visualizer's rendering library is tested here (docs/tasks/11-renderer.md §11.1); it's defined with the apps
if(NOT TARGET AVIS_VISUALIZER_RENDERING)
	message(FATAL_ERROR "The tests need the Visualizer's rendering library: configure with AVIS_INCLUDE_APPS=ON")
endif()

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
		AVIS_VISUALIZER_RENDERING

		# Private dependencies of AVIS_LIB, linked directly: D3D12MemoryAllocator for the RHI white-box tests (they
		# include lib/avis/rhi/d3d12/ headers), the others for test_external_dependencies.cpp
		D3D12MemoryAllocator
		fastgltf::fastgltf
		xxHash::xxhash
		yaml-cpp::yaml-cpp

		Catch2::Catch2WithMain)

# White-box tests include private library headers (lib/avis/...), e.g. the IO operation registry. Tests in
# subdirectories (visualizer/rendering/) include the helpers as "helpers/<file>.h" too.
target_include_directories(AVIS_TESTS
	PRIVATE
		${CMAKE_SOURCE_DIR}/lib
		${CMAKE_CURRENT_SOURCE_DIR})

target_sources(AVIS_TESTS
	PRIVATE
		helpers/allocation_failure.cpp
		helpers/graphics_fixture.cpp
		helpers/renderer_fixture.cpp
		helpers/rhi_fixture.cpp
		test_asio_senders_receivers.cpp
		test_async_file.cpp
		test_check.cpp
		test_enum_flags.cpp
		test_error.cpp
		test_external_dependencies.cpp
		test_file_operations.cpp
		test_finite_state_machine.cpp
		test_graphics_binding_layout.cpp
		test_graphics_error.cpp
		test_graphics_frame_scheduler.cpp
		test_graphics_gpu_completion_thread.cpp
		test_graphics_gpu_profiler.cpp
		test_graphics_linear_arena.cpp
		test_graphics_pipeline_cache.cpp
		test_graphics_public_headers.cpp
		test_graphics_readback_queue.cpp
		test_graphics_root_signature_registry.cpp
		test_graphics_shader_build.cpp
		test_graphics_shader_library.cpp
		test_graphics_shader_loading.cpp
		test_graphics_upload_queue.cpp
		test_helpers.cpp
		test_io_context.cpp
		test_io_error.cpp
		test_io_public_headers.cpp
		test_log.cpp
		test_mapped_file.cpp
		test_observables.cpp
		test_operation_registry.cpp
		test_parallel_backend.cpp
		test_render_graph_aliasing.cpp
		test_render_graph_barriers.cpp
		test_render_graph_build.cpp
		test_render_graph_compile.cpp
		test_render_graph_execute.cpp
		test_render_graph_frame.cpp
		test_rhi_command_lists.cpp
		test_rhi_completions.cpp
		test_rhi_conversions.cpp
		test_rhi_descriptor_heaps.cpp
		test_rhi_device_removal.cpp
		test_rhi_device.cpp
		test_rhi_leaf_mutex.cpp
		test_rhi_lifetime.cpp
		test_rhi_pipelines.cpp
		test_rhi_public_headers.cpp
		test_rhi_resources.cpp
		test_rhi_sharing.cpp
		test_rhi_slot_map.cpp
		test_rhi_stress.cpp
		test_rhi_transient.cpp
		test_rhi_types.cpp
		test_runtime.cpp
		test_runtime_configuration.cpp
		test_scene_asset_cache.cpp
		test_scene_bounds.cpp
		test_scene_components.cpp
		test_scene_error.cpp
		test_scene_gltf.cpp
		test_scene_graph.cpp
		test_scene_mesh_data.cpp
		test_scene_public_headers.cpp
		test_scene_reflection.cpp
		test_scene_snapshot.cpp
		test_scene_snapshot_buffer.cpp
		test_scene_statistics.cpp
		test_scene_transforms.cpp
		test_scene.cpp
		test_scene_types.cpp
		test_scene_viewing_aids.cpp
		test_sender_of.cpp
		test_thread_pool.cpp
		test_transient_resource_pool.cpp
		visualizer/rendering/test_extraction.cpp
		visualizer/rendering/test_geometry_arena.cpp
		visualizer/rendering/test_render_settings.cpp
		visualizer/rendering/test_renderer_frames.cpp
		visualizer/rendering/test_renderer_materials.cpp
		visualizer/rendering/test_renderer_shaders.cpp
		visualizer/rendering/test_residency_cache.cpp
		visualizer/rendering/test_shader_manifest_parser.cpp
		visualizer/rendering/test_view.cpp)

# RHI test shaders (docs/tasks/06-rhi.md §6.8): compiled at build time into headers with the bytecode as byte arrays,
# so the tests need no shader library. Generated, never checked in, rebuilt when the HLSL changes.
set(AVIS_TEST_DXC "${CMAKE_SOURCE_DIR}/tools/dxc/bin/x64/dxc.exe")
if(NOT EXISTS "${AVIS_TEST_DXC}")
	message(FATAL_ERROR "The RHI tests compile their shaders with DXC, expected at ${AVIS_TEST_DXC}")
endif()
set(AVIS_TEST_SHADER_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders")

function(avis_test_shader source entry profile name)
	set(input "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}")
	set(output "${AVIS_TEST_SHADER_DIRECTORY}/${name}.h")
	add_custom_command(
		OUTPUT "${output}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${AVIS_TEST_SHADER_DIRECTORY}"
		COMMAND "${AVIS_TEST_DXC}" -T ${profile} -E ${entry} -HV 2021 -WX -Fh "${output}" -Vn ${name} "${input}"
		DEPENDS "${input}"
		COMMENT "Compiling test shader ${name}"
		VERBATIM)
	target_sources(AVIS_TESTS PRIVATE "${output}")
endfunction()

avis_test_shader(rhi_test_triangle.hlsl vertex_main vs_6_6 rhi_test_vertex_shader)
avis_test_shader(rhi_test_triangle.hlsl pixel_main ps_6_6 rhi_test_pixel_shader)
avis_test_shader(rhi_test_fill.hlsl fill_main cs_6_6 rhi_test_compute_shader)

# Graphics-layer test shaders (docs/tasks/08-graphics-shaders-and-pipelines.md): bytecode headers until avis_add_shaders
# builds them with a manifest (task 8.7)
avis_test_shader(graphics/binding_layout_test.hlsl main cs_6_6 graphics_test_binding_layout_shader)
avis_test_shader(graphics/pipeline_test_draw.hlsl vertex_main vs_6_6 graphics_test_draw_vertex_shader)
avis_test_shader(graphics/pipeline_test_draw.hlsl pixel_main ps_6_6 graphics_test_draw_pixel_shader)
avis_test_shader(graphics/pipeline_test_buffers.hlsl main cs_6_6 graphics_test_buffers_shader)

# The same graphics test shaders built the way applications build theirs (docs/tasks/08-graphics-shaders-and-
# pipelines.md §8.7): .cso files and a manifest.yaml with hashes and reflected bindings, which
# test_graphics_shader_build.cpp loads from AVIS_TEST_BUILT_SHADER_DIRECTORY
set(AVIS_TEST_BUILT_SHADER_DIRECTORY "${CMAKE_BINARY_DIR}/bin/data/shaders/tests")
avis_add_shaders(
	TARGET AVIS_TESTS
	SOURCES
		shaders/graphics/binding_layout_test.hlsl
		shaders/graphics/pipeline_test_buffers.hlsl
		shaders/graphics/pipeline_test_draw.hlsl
	OUTPUT_DIRECTORY "${AVIS_TEST_BUILT_SHADER_DIRECTORY}")
target_compile_definitions(AVIS_TESTS
	PRIVATE
		AVIS_TEST_BUILT_SHADER_DIRECTORY="${AVIS_TEST_BUILT_SHADER_DIRECTORY}")

# The renderer's shaders as the Visualizer build deploys them: the rendering tests parse their manifest and load them
get_target_property(AVIS_TEST_RENDERER_SHADER_DIRECTORY AVIS_VISUALIZER_RENDERING AVIS_SHADER_OUTPUT_DIRECTORY)
target_compile_definitions(AVIS_TESTS
	PRIVATE
		AVIS_TEST_RENDERER_SHADER_DIRECTORY="${AVIS_TEST_RENDERER_SHADER_DIRECTORY}")

# test_scene_gltf.cpp loads its fixtures (Khronos sample models, hand-written invalid files) from the source tree
target_compile_definitions(AVIS_TESTS
	PRIVATE
		AVIS_TEST_GLTF_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/data/gltf")

target_include_directories(AVIS_TESTS
	PRIVATE
		"${AVIS_TEST_SHADER_DIRECTORY}")

# The public-header check (test_rhi_public_headers.cpp) includes every header under include/avis/rhi/, from a list
# generated here, so a new header can't be left out. CONFIGURE_DEPENDS re-runs the glob on each build; file(CONFIGURE)
# rewrites the list only when it changes.
file(GLOB_RECURSE AVIS_RHI_PUBLIC_HEADERS CONFIGURE_DEPENDS
	RELATIVE "${AVIS_INCLUDE_DIR}"
	"${AVIS_INCLUDE_DIR}/avis/rhi/*.h")
list(SORT AVIS_RHI_PUBLIC_HEADERS)
list(LENGTH AVIS_RHI_PUBLIC_HEADERS AVIS_RHI_PUBLIC_HEADER_COUNT)
if(AVIS_RHI_PUBLIC_HEADER_COUNT EQUAL 0)
	message(FATAL_ERROR "No public RHI headers found under ${AVIS_INCLUDE_DIR}/avis/rhi")
endif()
set(AVIS_RHI_PUBLIC_HEADER_INCLUDES "")
foreach(header IN LISTS AVIS_RHI_PUBLIC_HEADERS)
	string(APPEND AVIS_RHI_PUBLIC_HEADER_INCLUDES "#include \"${header}\"
")
endforeach()
set(AVIS_TEST_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE
	OUTPUT "${AVIS_TEST_GENERATED_DIRECTORY}/rhi_public_headers.h"
	CONTENT "// Generated by tests/CMakeLists.txt: every header under include/avis/rhi/
${AVIS_RHI_PUBLIC_HEADER_INCLUDES}"
	@ONLY)

# The same check for the graphics layer (test_graphics_public_headers.cpp), over every header in include/avis/graphics/.
file(GLOB_RECURSE AVIS_GRAPHICS_PUBLIC_HEADERS CONFIGURE_DEPENDS
	RELATIVE "${AVIS_INCLUDE_DIR}"
	"${AVIS_INCLUDE_DIR}/avis/graphics/*.h")
list(SORT AVIS_GRAPHICS_PUBLIC_HEADERS)
list(LENGTH AVIS_GRAPHICS_PUBLIC_HEADERS AVIS_GRAPHICS_PUBLIC_HEADER_COUNT)
if(AVIS_GRAPHICS_PUBLIC_HEADER_COUNT EQUAL 0)
	message(FATAL_ERROR "No public graphics headers found under ${AVIS_INCLUDE_DIR}/avis/graphics")
endif()
set(AVIS_GRAPHICS_PUBLIC_HEADER_INCLUDES "")
foreach(header IN LISTS AVIS_GRAPHICS_PUBLIC_HEADERS)
	string(APPEND AVIS_GRAPHICS_PUBLIC_HEADER_INCLUDES "#include \"${header}\"
")
endforeach()
file(CONFIGURE
	OUTPUT "${AVIS_TEST_GENERATED_DIRECTORY}/graphics_public_headers.h"
	CONTENT "// Generated by tests/CMakeLists.txt: every header under include/avis/graphics/
${AVIS_GRAPHICS_PUBLIC_HEADER_INCLUDES}"
	@ONLY)

# The same check for the scene library (test_scene_public_headers.cpp), over every header in include/avis/scene/ and
# include/avis/simulation/ (docs/tasks/10-scene-management.md §10.1).
file(GLOB_RECURSE AVIS_SCENE_PUBLIC_HEADERS CONFIGURE_DEPENDS
	RELATIVE "${AVIS_INCLUDE_DIR}"
	"${AVIS_INCLUDE_DIR}/avis/scene/*.h"
	"${AVIS_INCLUDE_DIR}/avis/simulation/*.h")
list(SORT AVIS_SCENE_PUBLIC_HEADERS)
list(LENGTH AVIS_SCENE_PUBLIC_HEADERS AVIS_SCENE_PUBLIC_HEADER_COUNT)
if(AVIS_SCENE_PUBLIC_HEADER_COUNT EQUAL 0)
	message(FATAL_ERROR "No public scene headers found under ${AVIS_INCLUDE_DIR}/avis/scene")
endif()
set(AVIS_SCENE_PUBLIC_HEADER_INCLUDES "")
foreach(header IN LISTS AVIS_SCENE_PUBLIC_HEADERS)
	string(APPEND AVIS_SCENE_PUBLIC_HEADER_INCLUDES "#include \"${header}\"
")
endforeach()
file(CONFIGURE
	OUTPUT "${AVIS_TEST_GENERATED_DIRECTORY}/scene_public_headers.h"
	CONTENT "// Generated by tests/CMakeLists.txt: every header under include/avis/scene/ and include/avis/simulation/
${AVIS_SCENE_PUBLIC_HEADER_INCLUDES}"
	@ONLY)

target_include_directories(AVIS_TESTS
	PRIVATE
		"${AVIS_TEST_GENERATED_DIRECTORY}")

include(CTest)
add_test(NAME "Avis Tests" COMMAND AVIS_TESTS)