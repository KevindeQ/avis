# avis_add_shaders: compiles HLSL with DXC and writes a shader manifest (docs/design/13-build-and-quality.md §3,
# docs/design/18-graphics-shaders-and-pipelines.md §6).
#
#   avis_add_shaders(TARGET <target> SOURCES <file.hlsl>... OUTPUT_DIRECTORY <directory>
#                    [DEFINES <NAME[=VALUE]>...] [INCLUDE_DIRECTORIES <directory>...])
#
# Each source declares its entry points in header comments, one or more per line:
#
#   // @avis-shader entry=main stage=cs
#   // @avis-shader entry=vertex_main stage=vs  entry=pixel_main stage=ps  permutations=ALPHA_MASK
#
# `stage` is vs, ps or cs and belongs to the entry before it. `permutations` (comma-separated) applies to every entry
# of its line: each entry is compiled once without them and once per permutation, with that name defined. The shader
# id is <file name>.<entry>[.<permutation in lower case>].<stage>, and its bytecode is <id>.cso in OUTPUT_DIRECTORY.
# A malformed comment fails the configure step with the file and line.
#
# Per shader, the reflection goes to <current binary dir>/shaders/<target>/reflection/<id>.ref (never deployed).
# shaders.yaml next to it lists every shader (avis_shader_list: 1), and a final step runs `dt shader-manifest`
# (tools/data-trace, through uv) to write OUTPUT_DIRECTORY/manifest.yaml with the hashes and bindings. The target
# depends on that step. The configure step fails when uv or tools/dxc is missing.

include_guard(GLOBAL)

# ---- script mode: the depfile rewrite run before each compile ----
# DXC's -M names the source file as the target, while Ninja expects the depfile to name the edge's output; otherwise it
# treats the shader as always dirty. This rewrites the target to the .cso and the separators to '/'.
if(CMAKE_SCRIPT_MODE_FILE AND DEFINED AVIS_SHADER_DEPFILE_INPUT)
	file(READ "${AVIS_SHADER_DEPFILE_INPUT}" dependencies)
	string(FIND "${dependencies}" ": " separator)
	if(separator EQUAL -1)
		message(FATAL_ERROR "avis_add_shaders: unexpected DXC dependency file ${AVIS_SHADER_DEPFILE_INPUT}")
	endif()
	math(EXPR separator "${separator} + 2")
	string(SUBSTRING "${dependencies}" ${separator} -1 dependencies)
	string(REPLACE "\\\n" "<continuation>" dependencies "${dependencies}")
	string(REPLACE "\\" "/" dependencies "${dependencies}")
	string(REPLACE "<continuation>" "\\\n" dependencies "${dependencies}")
	file(WRITE "${AVIS_SHADER_DEPFILE_OUTPUT}" "${AVIS_SHADER_DEPFILE_TARGET}: ${dependencies}")
	return()
endif()

set(AVIS_SHADERS_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")
set(AVIS_SHADERS_SOURCE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/..")
cmake_path(NORMAL_PATH AVIS_SHADERS_SOURCE_DIRECTORY)

# Finds DXC and uv once, failing the configure step with what to install
function(_avis_shader_tools)
	set(dxc_directory "${AVIS_SHADERS_SOURCE_DIRECTORY}/tools/dxc/bin/x64")
	if(NOT EXISTS "${dxc_directory}/dxc.exe" OR NOT EXISTS "${dxc_directory}/dxcompiler.dll")
		message(FATAL_ERROR
			"avis_add_shaders: DXC is missing. Expected dxc.exe and dxcompiler.dll in ${dxc_directory} "
			"(the DirectX Shader Compiler release unpacked into tools/dxc).")
	endif()
	find_program(AVIS_UV_EXECUTABLE uv)
	if(NOT AVIS_UV_EXECUTABLE)
		message(FATAL_ERROR
			"avis_add_shaders: uv is missing. The shader manifest is written by tools/data-trace, which runs through "
			"uv: install it (https://docs.astral.sh/uv/) and configure again.")
	endif()
	set(AVIS_DXC_DIRECTORY "${dxc_directory}" PARENT_SCOPE)
endfunction()

# Parses the @avis-shader comments of `source` into a list of "entry|stage|permutation,..." records in `out`
function(_avis_parse_shader_header source out)
	file(READ "${source}" content)
	# One list element per line. Characters CMake's list splitting treats specially are masked first: only the
	# @avis-shader lines are looked at, and they may not contain them anyway.
	string(REPLACE ";" "<semicolon>" content "${content}")
	string(REPLACE "[" "<open>" content "${content}")
	string(REPLACE "]" "<close>" content "${content}")
	string(REPLACE "\\" "<backslash>" content "${content}")
	string(REPLACE "\r" "" content "${content}")
	string(REPLACE "\n" ";" lines "${content}")

	set(records "")
	set(line_number 0)
	foreach(line IN LISTS lines)
		math(EXPR line_number "${line_number} + 1")
		if(NOT line MATCHES "^[ \t]*//[ \t]*@avis-shader(.*)$")
			continue()
		endif()
		set(where "${source}:${line_number}")
		string(STRIP "${CMAKE_MATCH_1}" arguments)
		if(arguments STREQUAL "")
			message(FATAL_ERROR "${where}: @avis-shader without an entry")
		endif()
		string(REGEX REPLACE "[ \t]+" ";" tokens "${arguments}")

		set(entries "")
		set(stages "")
		set(permutations "")
		set(permutations_seen FALSE)
		foreach(token IN LISTS tokens)
			if(NOT token MATCHES "^([a-z]+)=(.*)$")
				message(FATAL_ERROR "${where}: '${token}' isn't key=value (expected entry=, stage= or permutations=)")
			endif()
			set(key "${CMAKE_MATCH_1}")
			set(value "${CMAKE_MATCH_2}")
			if(key STREQUAL "entry")
				if(NOT value MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
					message(FATAL_ERROR "${where}: entry '${value}' isn't an HLSL identifier")
				endif()
				list(LENGTH entries entry_count)
				list(LENGTH stages stage_count)
				if(NOT entry_count EQUAL stage_count)
					list(GET entries -1 previous)
					message(FATAL_ERROR "${where}: entry '${previous}' has no stage= before the next entry")
				endif()
				list(APPEND entries "${value}")
			elseif(key STREQUAL "stage")
				list(LENGTH entries entry_count)
				list(LENGTH stages stage_count)
				math(EXPR expected_entries "${stage_count} + 1")
				if(NOT entry_count EQUAL expected_entries)
					message(FATAL_ERROR "${where}: stage=${value} doesn't follow an entry= without a stage")
				endif()
				if(NOT value MATCHES "^(vs|ps|cs)$")
					message(FATAL_ERROR "${where}: stage '${value}' isn't vs, ps or cs")
				endif()
				list(APPEND stages "${value}")
			elseif(key STREQUAL "permutations")
				if(permutations_seen)
					message(FATAL_ERROR "${where}: permutations= appears twice")
				endif()
				set(permutations_seen TRUE)
				string(REPLACE "," ";" permutations "${value}")
				if(permutations STREQUAL "")
					message(FATAL_ERROR "${where}: permutations= has no names")
				endif()
				foreach(permutation IN LISTS permutations)
					if(NOT permutation MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
						message(FATAL_ERROR "${where}: permutation '${permutation}' isn't a valid define name")
					endif()
				endforeach()
			else()
				message(FATAL_ERROR "${where}: unknown key '${key}' (expected entry=, stage= or permutations=)")
			endif()
		endforeach()

		list(LENGTH entries entry_count)
		list(LENGTH stages stage_count)
		if(entry_count EQUAL 0)
			message(FATAL_ERROR "${where}: @avis-shader without an entry")
		endif()
		if(NOT entry_count EQUAL stage_count)
			list(GET entries -1 previous)
			message(FATAL_ERROR "${where}: entry '${previous}' has no stage=")
		endif()
		string(REPLACE ";" "," permutation_text "${permutations}")
		math(EXPR last "${entry_count} - 1")
		foreach(index RANGE ${last})
			list(GET entries ${index} entry)
			list(GET stages ${index} stage)
			list(APPEND records "${entry}|${stage}|${permutation_text}")
		endforeach()
	endforeach()

	if(records STREQUAL "")
		message(FATAL_ERROR "${source}: no // @avis-shader comment declares an entry point")
	endif()
	set(${out} "${records}" PARENT_SCOPE)
endfunction()

function(avis_add_shaders)
	cmake_parse_arguments(PARSE_ARGV 0 arg "" "TARGET;OUTPUT_DIRECTORY" "SOURCES;DEFINES;INCLUDE_DIRECTORIES")
	if(arg_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "avis_add_shaders: unexpected arguments ${arg_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT arg_TARGET OR NOT TARGET "${arg_TARGET}")
		message(FATAL_ERROR "avis_add_shaders: TARGET must name an existing target, got '${arg_TARGET}'")
	endif()
	if(NOT arg_SOURCES)
		message(FATAL_ERROR "avis_add_shaders(${arg_TARGET}): no SOURCES")
	endif()
	if(NOT arg_OUTPUT_DIRECTORY)
		message(FATAL_ERROR "avis_add_shaders(${arg_TARGET}): no OUTPUT_DIRECTORY")
	endif()
	set(shaders_target "${arg_TARGET}_shaders")
	if(TARGET "${shaders_target}")
		message(FATAL_ERROR "avis_add_shaders: called twice for ${arg_TARGET}; pass all its shaders in one call")
	endif()

	_avis_shader_tools()

	cmake_path(ABSOLUTE_PATH arg_OUTPUT_DIRECTORY NORMALIZE OUTPUT_VARIABLE output_directory)
	set(work_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders/${arg_TARGET}")
	set(reflection_directory "${work_directory}/reflection")
	set(dependency_directory "${work_directory}/dependencies")
	set(pdb_directory "${output_directory}/pdb")
	set(list_file "${work_directory}/shaders.yaml")
	set(manifest "${output_directory}/manifest.yaml")
	set(dxc "${AVIS_DXC_DIRECTORY}/dxc.exe")

	set(common_options -HV 2021 -enable-16bit-types -WX)
	foreach(directory IN LISTS arg_INCLUDE_DIRECTORIES)
		cmake_path(ABSOLUTE_PATH directory NORMALIZE)
		list(APPEND common_options -I "${directory}")
	endforeach()
	foreach(define IN LISTS arg_DEFINES)
		list(APPEND common_options "-D${define}")
	endforeach()
	# Debug: embedded debug info and a PDB per shader; -Fd needs -Zi, so it's passed in Debug only
	# One generator expression per argument: a ';' inside one would split it before it's evaluated. Empty ones are
	# dropped by COMMAND_EXPAND_LISTS.
	set(debug "$<CONFIG:Debug>")
	set(configuration_options
		"$<${debug}:-Zi>" "$<${debug}:-Qembed_debug>" "$<${debug}:-Od>" "$<${debug}:-Fd>" "$<${debug}:${pdb_directory}/>"
		"$<$<NOT:${debug}>:-O3>")

	set(list_text "avis_shader_list: 1\nshaders:\n")
	set(outputs "")
	set(ids "")
	foreach(source IN LISTS arg_SOURCES)
		cmake_path(ABSOLUTE_PATH source NORMALIZE)
		if(NOT EXISTS "${source}")
			message(FATAL_ERROR "avis_add_shaders(${arg_TARGET}): ${source} doesn't exist")
		endif()
		cmake_path(GET source STEM name)
		_avis_parse_shader_header("${source}" records)

		foreach(record IN LISTS records)
			string(REPLACE "|" ";" fields "${record}")
			list(GET fields 0 entry)
			list(GET fields 1 stage)
			list(GET fields 2 permutation_text)
			string(REPLACE "," ";" permutations "${permutation_text}")

			# The entry without permutations first, then one variant per permutation
			foreach(permutation IN ITEMS "<none>" ${permutations})
				if(permutation STREQUAL "<none>")
					set(id "${name}.${entry}.${stage}")
					set(variant_defines "")
				else()
					string(TOLOWER "${permutation}" suffix)
					set(id "${name}.${entry}.${suffix}.${stage}")
					set(variant_defines "${permutation}")
				endif()
				if(id IN_LIST ids)
					message(FATAL_ERROR "avis_add_shaders(${arg_TARGET}): shader id ${id} is declared twice (${source})")
				endif()
				list(APPEND ids "${id}")

				set(bytecode "${output_directory}/${id}.cso")
				set(reflection "${reflection_directory}/${id}.ref")
				set(raw_dependencies "${dependency_directory}/${id}.dxc.d")
				set(dependencies "${dependency_directory}/${id}.d")
				set(variant_options "")
				foreach(define IN LISTS variant_defines)
					list(APPEND variant_options "-D${define}")
				endforeach()

				set(depfile_arguments "")
				if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
					set(depfile_arguments DEPFILE "${dependencies}")
				endif()
				add_custom_command(
					OUTPUT "${bytecode}" "${reflection}"
					COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_directory}" "${reflection_directory}"
						"${dependency_directory}" "${pdb_directory}"
					# Dependencies first: with -M, DXC lists the includes and compiles nothing
					COMMAND "${dxc}" -T ${stage}_6_6 -E ${entry} ${common_options} ${variant_options}
						-M -MF "${raw_dependencies}" "${source}"
					COMMAND "${CMAKE_COMMAND}" -DAVIS_SHADER_DEPFILE_INPUT=${raw_dependencies}
						-DAVIS_SHADER_DEPFILE_OUTPUT=${dependencies} -DAVIS_SHADER_DEPFILE_TARGET=${bytecode}
						-P "${AVIS_SHADERS_SCRIPT}"
					COMMAND "${dxc}" -T ${stage}_6_6 -E ${entry} ${common_options} ${variant_options}
						${configuration_options}
						-Fo "${bytecode}" -Fre "${reflection}" -Qstrip_reflect "${source}"
					DEPENDS "${source}"
					${depfile_arguments}
					COMMENT "Compiling shader ${id}"
					COMMAND_EXPAND_LISTS
					VERBATIM)
				list(APPEND outputs "${bytecode}" "${reflection}")

				# The list is YAML; every value is a single-quoted scalar
				set(define_items "")
				foreach(define IN LISTS arg_DEFINES variant_defines)
					list(APPEND define_items "'${define}'")
				endforeach()
				list(JOIN define_items ", " define_text)
				string(APPEND list_text
					"  - id: '${id}'\n"
					"    stage: ${stage}\n"
					"    entry: '${entry}'\n"
					"    defines: [${define_text}]\n"
					"    bytecode: '${bytecode}'\n"
					"    reflection: '${reflection}'\n")
			endforeach()
		endforeach()
	endforeach()

	# Rewritten only when its content changes, so an unchanged list doesn't rerun the manifest step
	file(GENERATE OUTPUT "${list_file}" CONTENT "${list_text}")

	# dt writes its debug.log to the working directory, so it runs in the build tree. It rewrites manifest.yaml only
	# when the content changes; Ninja's restat then keeps the manifest's dependents up to date.
	add_custom_command(
		OUTPUT "${manifest}"
		COMMAND "${AVIS_UV_EXECUTABLE}" run --project "${AVIS_SHADERS_SOURCE_DIRECTORY}/tools/data-trace"
			dt shader-manifest "${list_file}" --output "${manifest}" --dxc "${AVIS_DXC_DIRECTORY}"
		DEPENDS ${outputs} "${list_file}"
		WORKING_DIRECTORY "${work_directory}"
		COMMENT "Writing the shader manifest of ${arg_TARGET}"
		VERBATIM)

	add_custom_target(${shaders_target} DEPENDS "${manifest}")
	add_dependencies(${arg_TARGET} ${shaders_target})
	set_property(TARGET ${shaders_target} PROPERTY AVIS_SHADER_MANIFEST "${manifest}")
endfunction()
