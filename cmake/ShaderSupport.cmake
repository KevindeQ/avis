function(enable_build_shaders PARENT_TARGET)
	add_custom_target(shaders)
	add_dependencies(${PARENT_TARGET} shaders)
endfunction()

function(build_shaders SHADER_FILES SHADER_TYPE)
	foreach(FILE ${SHADER_FILES})
		get_filename_component(FILE_WE ${FILE} NAME_WE)
		add_custom_command(TARGET shaders
			COMMAND dxc.exe /nologo /Emain /T${SHADER_TYPE} $<IF:$<CONFIG:DEBUG>,/Od,/O3> /Zi /Fo ${CMAKE_BINARY_DIR}/${FILE_WE}.cso /Fd ${CMAKE_BINARY_DIR}/${FILE_WE}.pdb ${FILE}
		MAIN_DEPENDENCY ${FILE}
		COMMENT "HLSL ${FILE}"
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		VERBATIM)
	endforeach(FILE)
endfunction()