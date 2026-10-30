add_library(AVIS_LIB STATIC)

target_compile_features(AVIS_LIB
	PUBLIC cxx_std_${AVIS_REQUIRED_CXX_STANDARD})

set_target_properties(AVIS_LIB
	PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"

		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)

target_compile_definitions(AVIS_LIB
	PUBLIC
		UNICODE
		_UNICODE

		# Windows 10+ APIs (the minimum OS is Windows 11, D-05). Without it asio assumes Windows 7 (0x0601) and hides
		# newer APIs from <windows.h>, e.g. FileRenameInfoEx for POSIX-semantics renames.
		_WIN32_WINNT=0x0A00

		# Disable the usage of Qt specific keywords since they interfere with commonly
		# used function names such as emit.
		QT_NO_KEYWORDS
)

# Third-party include directories come with the linked targets below
target_include_directories(AVIS_LIB
	PUBLIC
		${AVIS_INCLUDE_DIR}

	# Private helpers shared across components (lib/avis/detail/, e.g. "avis/detail/safe_error.h"), included the same
	# way as in the white-box tests
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(AVIS_LIB
	PUBLIC
		ASIO
		Eigen3::Eigen
		EnTT::EnTT
		h3
		STDEXEC::stdexec

		# Public because include/avis/log.h exposes spdlog::logger
		spdlog::spdlog

	PRIVATE
		# No public Avis header includes these. A static library's private dependencies still reach the executables
		# as link-only ones, so d3d12.lib, dxgi.lib and dxguid.lib (D3D12MemoryAllocator's public links) do too.
		D3D12MemoryAllocator
		fastgltf::fastgltf
		yaml-cpp::yaml-cpp
)

target_sources(AVIS_LIB
	PRIVATE
		detail/check.cpp

		error.cpp
		log.cpp
		runtime.cpp
		runtime_configuration.cpp
		virtual_clock_state.cpp

		graphics/error.cpp
		graphics/frame_scheduler.cpp
		graphics/gpu_profiler.cpp

		io/async_file.cpp
		io/detail/file_sender.cpp
		io/detail/operation_registry.cpp
		io/error.cpp
		io/file_operations.cpp
		io/io_context.cpp
		io/mapped_file.cpp

		rhi/command_list.cpp
		rhi/completion_driver.cpp
		rhi/error.cpp
		rhi/formats.cpp
		rhi/memory.cpp
		rhi/pipeline.cpp
		rhi/queue.cpp
		rhi/render_hardware_interface.cpp
		rhi/resources.cpp
		rhi/shared.cpp
		rhi/synchronization.cpp
		rhi/transient.cpp
		rhi/d3d12/adapter_selector.cpp
		rhi/d3d12/capability_gate.cpp
		rhi/d3d12/check.cpp
		rhi/d3d12/command_list_pool.cpp
		rhi/d3d12/completion_tracker.cpp
		rhi/d3d12/conversions/barriers.cpp
		rhi/d3d12/conversions/formats.cpp
		rhi/d3d12/conversions/pipelines.cpp
		rhi/d3d12/conversions/queues.cpp
		rhi/d3d12/conversions/resources.cpp
		rhi/d3d12/conversions/samplers.cpp
		rhi/d3d12/debug.cpp
		rhi/d3d12/deferred_release.cpp
		rhi/d3d12/descriptor_heaps.cpp
		rhi/d3d12/device.cpp
		rhi/d3d12/device_queues.cpp
		rhi/d3d12/device_state.cpp
		rhi/d3d12/gpu_wait_operation.cpp
		rhi/d3d12/lifetime_manager.cpp
		rhi/d3d12/memory_allocator.cpp
		rhi/d3d12/pipeline_factory.cpp
		rhi/d3d12/resource_manager.cpp
		rhi/d3d12/root_signature.cpp
		rhi/d3d12/shared_objects.cpp
		rhi/d3d12/transient_upload.cpp
		rhi/d3d12/view_descriptions.cpp

		threading/parallel_backend.cpp
		threading/parallel_helpers.cpp
		threading/pool_thread.cpp
		threading/thread_handle.cpp
		threading/thread_pool.cpp
		threading/thread_priority.cpp
		threading/thread_state.cpp
)
