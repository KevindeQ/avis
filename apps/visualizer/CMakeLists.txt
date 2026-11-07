# The Qt-free parts are static libraries, so the tests can link them (docs/tasks/11-renderer.md §11.1,
# docs/tasks/12-visualizer.md §12.1): the scene renderer, the scene representation, and the simulation side with the
# configuration. Rendering comes first: it defines the include root and the no-Qt check the others use.
add_subdirectory(rendering)
add_subdirectory(representation)
add_subdirectory(simulation)

qt_add_executable(AVIS_VISUALIZER
	main.cpp
	visualizer_driver.cpp
	ui/simulation_bridge.cpp
	ui/simulation_bridge.h
	ui/viewport_renderer.cpp
)

# qml/main.qml is served as main.qml at the module's root, next to its qmldir, so it sees the module's types
set_source_files_properties(qml/main.qml PROPERTIES QT_RESOURCE_ALIAS main.qml)

qt_add_qml_module(AVIS_VISUALIZER
    URI Avis.Visualizer
    RESOURCE_PREFIX /avis/imports
	QML_FILES
        qml/main.qml
	SOURCES
		ui/viewport_item.cpp
		ui/viewport_item.h
)

# The Qt-side blit shaders are Visualizer data (11 §13); their resource paths stay :/avis/visualizer/shaders/<file>.qsb
set(AVIS_VISUALIZER_QT_SHADER_DIRECTORY "${AVIS_SRC_DIR}/data/shaders/visualizer/qt")
qt6_add_shaders(AVIS_VISUALIZER "shaders"
    HLSL 61
	GLSL "460"
	NOMSL
	OPTIMIZED
    PREFIX
        "/avis/visualizer/shaders"
    BASE
        "${AVIS_VISUALIZER_QT_SHADER_DIRECTORY}"
    FILES
        "${AVIS_VISUALIZER_QT_SHADER_DIRECTORY}/fullscreen_triangle.vert"
        "${AVIS_VISUALIZER_QT_SHADER_DIRECTORY}/fullscreen_texture.frag"
)

target_compile_features(AVIS_VISUALIZER
	PUBLIC cxx_std_${AVIS_REQUIRED_CXX_STANDARD})

set_target_properties(AVIS_VISUALIZER
	PROPERTIES
		OUTPUT_NAME "visualizer"
		WIN32_EXECUTABLE TRUE

		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)

target_compile_definitions(AVIS_VISUALIZER
	PUBLIC
		AVIS_LIB
	PRIVATE
		$<$<CONFIG:debug>:QT_QML_DEBUG>
)

target_include_directories(AVIS_VISUALIZER
	PRIVATE
		AVIS_LIB

		# The QML type registration includes each QML_ELEMENT header by its bare name (<viewport_item.h>)
		"${CMAKE_CURRENT_SOURCE_DIR}/ui"

		EnTT::EnTT
)

target_link_libraries(AVIS_VISUALIZER
	PRIVATE
		AVIS_LIB
		AVIS_VISUALIZER_RENDERING
		AVIS_VISUALIZER_REPRESENTATION
		AVIS_VISUALIZER_SIMULATION

		EnTT::EnTT
		Qt6::Gui
		Qt6::GuiPrivate
		Qt6::Quick

		# ui/viewport_renderer.cpp: the debug layer before Qt's device, and the shared frame ring's handles
		d3d12
)

qt_deploy_windows(AVIS_VISUALIZER ${CMAKE_CURRENT_SOURCE_DIR})
