# Get the absolute path to qmake, then use it to find windeployqt executable
get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)

function(qt_deploy_windows target qml_dir)

    # POST_BUILD step
    # - after build, analyze bin/lib for qt dependencies
    # - run windeployqt on target and deploy Qt libs
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${_qt_bin_dir}/windeployqt.exe"         
                --verbose 1
                $<IF:$<CONFIG:debug>,--debug,--release>
                --no-opengl-sw
                --no-compiler-runtime
                --no-system-d3d-compiler
                --no-system-dxc-compiler
                --qmldir ${qml_dir}
                \"$<TARGET_FILE:${target}>\"
        COMMENT "Deploying Qt libraries using windeployqt for compilation target '${target}' ..."
    )

endfunction()