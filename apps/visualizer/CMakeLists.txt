qt_add_executable(AVIS_VISUALIZER
	main.cpp
	visualizer_driver.cpp
	rendering/scene_renderer.cpp
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

qt6_add_shaders(AVIS_VISUALIZER "shaders"
    HLSL 61
	GLSL "460"
	NOMSL
	OPTIMIZED
    PREFIX
        "/avis/visualizer"
    FILES
        "shaders/fullscreen_triangle.vert"
        "shaders/fullscreen_texture.frag"
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

		EnTT::EnTT
		Qt6::Gui
		Qt6::GuiPrivate
		Qt6::Quick
)

qt_deploy_windows(AVIS_VISUALIZER ${CMAKE_CURRENT_SOURCE_DIR})
