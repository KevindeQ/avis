# AVIS_VISUALIZER_UI_TESTS (docs/tasks/12-visualizer.md, Conventions): Qt Test classes for the Visualizer's UI side.
# Catch2 can't host them, since they need a QGuiApplication and, for the interop checks, Qt's D3D12 device.
qt_add_executable(AVIS_VISUALIZER_UI_TESTS
	main.cpp
	ui_tests.h
	test_qt_interop.cpp
	test_viewport_renderer.cpp

	# The Visualizer's viewport, tested as it is: these two sources are Qt-side and live in the executable, so they're
	# compiled here too (the item's QML registration stays with the application)
	${AVIS_SRC_DIR}/apps/visualizer/ui/viewport_item.cpp
	${AVIS_SRC_DIR}/apps/visualizer/ui/viewport_renderer.cpp
)

# The Visualizer's Qt-side blit shaders, under the same resource paths as in the application
set(AVIS_VISUALIZER_QT_SHADER_DIRECTORY "${AVIS_SRC_DIR}/data/shaders/visualizer/qt")
qt6_add_shaders(AVIS_VISUALIZER_UI_TESTS "ui_test_shaders"
	HLSL 61
	NOGLSL
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

target_compile_features(AVIS_VISUALIZER_UI_TESTS
	PUBLIC cxx_std_${AVIS_REQUIRED_CXX_STANDARD})

set_target_properties(AVIS_VISUALIZER_UI_TESTS
	PROPERTIES
		OUTPUT_NAME "avis_visualizer_ui_tests"
		WIN32_EXECUTABLE FALSE # qt_add_executable makes a GUI program; the test output goes to the console

		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)

target_link_libraries(AVIS_VISUALIZER_UI_TESTS
	PRIVATE
		AVIS_LIB
		AVIS_VISUALIZER_RENDERING # the shared frame ring; its include root gives "ui/..." too

		Qt6::Gui
		Qt6::GuiPrivate # rhi/qrhi.h
		Qt6::Quick
		Qt6::Test

		d3d12)

# The shared test helpers, as "helpers/<file>.h"
target_include_directories(AVIS_VISUALIZER_UI_TESTS
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/..)

qt_deploy_windows(AVIS_VISUALIZER_UI_TESTS ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME "Avis Visualizer UI Tests" COMMAND AVIS_VISUALIZER_UI_TESTS)
