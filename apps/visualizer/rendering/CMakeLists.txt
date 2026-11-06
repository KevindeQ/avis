# The Visualizer's scene renderer (docs/design/11-renderer.md, docs/tasks/11-renderer.md §11.1): everything in
# rendering/ that doesn't use Qt, as a static library that AVIS_VISUALIZER and AVIS_TESTS link. Its shaders are
# Visualizer data under data/shaders/visualizer/ (11 §13), compiled by avis_add_shaders next to the executable.

add_library(AVIS_VISUALIZER_RENDERING STATIC
	blue_noise.cpp
	cascades.cpp
	clusters.cpp
	draw_lists.cpp
	environment_baker.cpp
	extraction.cpp
	frame_data.cpp
	geometry_arena.cpp
	material_table.cpp
	passes/depth_passes.cpp
	passes/exposure_passes.cpp
	passes/forward_pass.cpp
	passes/gbuffer_pass.cpp
	passes/gtao_pass.cpp
	passes/lighting_passes.cpp
	passes/overlay_pass.cpp
	passes/scene_range_pass.cpp
	passes/shadow_pass.cpp
	passes/taa_pass.cpp
	pipeline_variant_registry.cpp
	range_walker.cpp
	render_settings.cpp
	renderer_shaders.cpp
	residency_cache.cpp
	scene_renderer.cpp
	texture_residency.cpp
	shader_manifest_parser.cpp
	sky_model.cpp
	sort_key.cpp
	view.cpp
	view_constants.cpp
	view_history.cpp
	yaml_reading.cpp)

target_compile_features(AVIS_VISUALIZER_RENDERING
	PUBLIC cxx_std_${AVIS_REQUIRED_CXX_STANDARD})

set_target_properties(AVIS_VISUALIZER_RENDERING
	PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF

		# No Qt here: qt_standard_project_setup turns these on for every target
		AUTOMOC OFF
		AUTOUIC OFF
		AUTORCC OFF)

set(AVIS_VISUALIZER_SHADER_SOURCE_DIRECTORY "${AVIS_SRC_DIR}/data/shaders/visualizer")

# Sources include "rendering/<file>.h"; shared_types.hlsli is included by name from both C++ and HLSL (11 §13)
target_include_directories(AVIS_VISUALIZER_RENDERING
	PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}/.."
		"${AVIS_VISUALIZER_SHADER_SOURCE_DIRECTORY}/include"

	# AVIS_CHECK ("avis/detail/check.h") for the renderer's debug checks (docs/tasks/11-renderer.md, Conventions)
	PRIVATE
		"${AVIS_SRC_DIR}/lib")

target_link_libraries(AVIS_VISUALIZER_RENDERING
	PUBLIC
		AVIS_LIB

		# Public because render_settings.h takes a YAML::Node
		yaml-cpp::yaml-cpp)

# The scene renderer's shaders: compiled into <bin>/data/shaders/visualizer with manifest.yaml, the directory
# resolve_shader_directory() finds next to the executable
set(AVIS_VISUALIZER_SHADER_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/data/shaders/visualizer")
file(GLOB AVIS_VISUALIZER_SCENE_SHADERS CONFIGURE_DEPENDS "${AVIS_VISUALIZER_SHADER_SOURCE_DIRECTORY}/scene/*.hlsl")
list(SORT AVIS_VISUALIZER_SCENE_SHADERS)
avis_add_shaders(
	TARGET AVIS_VISUALIZER_RENDERING
	SOURCES ${AVIS_VISUALIZER_SCENE_SHADERS}
	OUTPUT_DIRECTORY "${AVIS_VISUALIZER_SHADER_OUTPUT_DIRECTORY}"
	INCLUDE_DIRECTORIES "${AVIS_VISUALIZER_SHADER_SOURCE_DIRECTORY}/include")

# For the tests, which load the built shaders and parse the manifest the build wrote
set_property(TARGET AVIS_VISUALIZER_RENDERING
	PROPERTY AVIS_SHADER_OUTPUT_DIRECTORY "${AVIS_VISUALIZER_SHADER_OUTPUT_DIRECTORY}")

# The library must not depend on Qt (docs/tasks/11-renderer.md §11.1): no Qt6:: target anywhere in its link
# dependencies, direct or transitive. Checked once every directory is configured, so later links count too.
function(_avis_check_no_qt_dependency target)
	set(pending "${target}")
	set(visited "")
	while(pending)
		list(POP_FRONT pending current)
		if(current IN_LIST visited)
			continue()
		endif()
		list(APPEND visited "${current}")
		set(dependencies "")
		foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
			get_target_property(values "${current}" ${property})
			if(values)
				list(APPEND dependencies ${values})
			endif()
		endforeach()
		foreach(dependency IN LISTS dependencies)
			# $<LINK_ONLY:x> and similar wrappers name the target inside
			string(REGEX REPLACE "^\\$<[A-Z_]+:(.*)>$" "\\1" dependency "${dependency}")
			if(dependency MATCHES "^Qt6?::")
				message(FATAL_ERROR
					"${target} must not depend on Qt (docs/tasks/11-renderer.md §11.1), but ${current} links ${dependency}")
			endif()
			if(TARGET "${dependency}")
				get_target_property(imported "${dependency}" IMPORTED)
				get_target_property(aliased "${dependency}" ALIASED_TARGET)
				if(aliased)
					list(APPEND pending "${aliased}")
				elseif(NOT imported)
					list(APPEND pending "${dependency}")
				endif()
			endif()
		endforeach()
	endwhile()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _avis_check_no_qt_dependency AVIS_VISUALIZER_RENDERING)
