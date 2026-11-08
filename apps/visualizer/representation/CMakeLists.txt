# The scene representation (docs/design/12-visualizer.md §9, docs/tasks/12-visualizer.md §12.1): streamed content to
# meshes and materials, as a Qt-free static library that AVIS_VISUALIZER and AVIS_TESTS link, like the renderer's.

add_library(AVIS_VISUALIZER_REPRESENTATION STATIC
	helper_geometry.cpp
	material_cache.cpp
	presentation_state.cpp
	representation_settings.cpp
	representation_system.cpp
	shape_representation.cpp
	shared_assets.cpp)

target_compile_features(AVIS_VISUALIZER_REPRESENTATION
	PUBLIC cxx_std_${AVIS_REQUIRED_CXX_STANDARD})

set_target_properties(AVIS_VISUALIZER_REPRESENTATION
	PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF

		# No Qt here: qt_standard_project_setup turns these on for every target
		AUTOMOC OFF
		AUTOUIC OFF
		AUTORCC OFF)

# Sources include "representation/<file>.h"; the include root, apps/visualizer/, comes with the rendering library
target_include_directories(AVIS_VISUALIZER_REPRESENTATION
	PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}/.."

	# AVIS_CHECK ("avis/detail/check.h") for the debug checks (docs/tasks/12-visualizer.md, Conventions)
	PRIVATE
		"${AVIS_SRC_DIR}/lib")

# Public because representation_settings.h takes a YAML::Node
target_link_libraries(AVIS_VISUALIZER_REPRESENTATION
	PUBLIC
		AVIS_LIB
		AVIS_VISUALIZER_RENDERING
		yaml-cpp::yaml-cpp)

# Defined by rendering/CMakeLists.txt: no Qt6:: target anywhere in the link dependencies
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _avis_check_no_qt_dependency AVIS_VISUALIZER_REPRESENTATION)
