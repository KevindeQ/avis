# The scene renderer: a Qt-free static library, so the tests can link it (docs/tasks/11-renderer.md §11.1)
add_subdirectory(rendering)

qt_add_executable(AVIS_VISUALIZER
	main.cpp
	visualizer_driver.cpp
)


qt_add_qml_module(AVIS_VISUALIZER
    URI visualizer
    RESOURCE_PREFIX /avis/imports
	QML_FILES
        main.qml
	SOURCES
		viewport.cpp
		viewport.h
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

		EnTT::EnTT
)

target_link_libraries(AVIS_VISUALIZER
	PRIVATE
		AVIS_LIB
		AVIS_VISUALIZER_RENDERING

		EnTT::EnTT
		Qt6::Gui
		Qt6::GuiPrivate
		Qt6::Quick
)

qt_deploy_windows(AVIS_VISUALIZER ${CMAKE_CURRENT_SOURCE_DIR})
