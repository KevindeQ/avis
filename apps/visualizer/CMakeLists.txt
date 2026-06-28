qt_add_executable(AVIS_VISUALIZER
	field_reader.cpp
    main.cpp
	scene_provider.cpp
	scene_provider_renderer.cpp
	sensor_injector.cpp
	simulator_driver.cpp
)

qt_add_qml_module(AVIS_VISUALIZER
    URI visualizer_qt
	RESOURCE_PREFIX /
    QML_FILES
        main.qml
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
)

target_link_libraries(AVIS_VISUALIZER
	PRIVATE
		Qt6::Gui
		Qt6::Quick

		AVIS_LIB
)

qt_deploy_windows(AVIS_VISUALIZER ${CMAKE_CURRENT_SOURCE_DIR})
