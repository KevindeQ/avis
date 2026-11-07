# The Visualizer's simulation side (docs/design/12-visualizer.md §2, §4, docs/tasks/12-visualizer.md §12.1): the
# simulation thread and what it drives, as a Qt-free static library that AVIS_VISUALIZER and AVIS_TESTS link. It also
# holds the application's configuration (visualizer_configuration.h, in apps/visualizer/), which reads the settings of
# every other part and must be testable without Qt.

add_library(AVIS_VISUALIZER_SIMULATION STATIC
	../visualizer_configuration.cpp)

target_compile_features(AVIS_VISUALIZER_SIMULATION
	PUBLIC cxx_std_${AVIS_REQUIRED_CXX_STANDARD})

set_target_properties(AVIS_VISUALIZER_SIMULATION
	PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF

		# No Qt here: qt_standard_project_setup turns these on for every target
		AUTOMOC OFF
		AUTOUIC OFF
		AUTORCC OFF)

# Sources include "simulation/<file>.h" and "visualizer_configuration.h" from apps/visualizer/
target_include_directories(AVIS_VISUALIZER_SIMULATION
	PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}/.."

	# AVIS_CHECK ("avis/detail/check.h") for the debug checks (docs/tasks/12-visualizer.md, Conventions)
	PRIVATE
		"${AVIS_SRC_DIR}/lib")

target_link_libraries(AVIS_VISUALIZER_SIMULATION
	PUBLIC
		AVIS_LIB
		AVIS_VISUALIZER_RENDERING
		AVIS_VISUALIZER_REPRESENTATION
		yaml-cpp::yaml-cpp)

# Defined by rendering/CMakeLists.txt: no Qt6:: target anywhere in the link dependencies
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _avis_check_no_qt_dependency AVIS_VISUALIZER_SIMULATION)
