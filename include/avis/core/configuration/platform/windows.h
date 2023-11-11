#ifndef CORE_CONFIGURATION_PLATFORM_WINDOWS_H
#define CORE_CONFIGURATION_PLATFORM_WINDOWS_H

// Windows specific libraries
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3d12.lib")

// Windows specific definitions
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif

#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif

#if defined(ENGINE_BUILD_DEBUG)
#    define _CRTDBG_MAP_ALLOC
#endif

// Windows specific include files
#include <crtdbg.h>
#include <cstdlib>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <wrl/client.h>
#include <xmmintrin.h> // SSE / SSE2 intrinsics

#include <Windows.h>

// Control for exporting the engine API from its dll
#if defined(ENGINE_BUILD_SHARED)
#    if defined(ENGINE_EXPORT_API)
#        define ENGINE_API __declspec(dllexport)
#    else
#        define ENGINE_API __declspec(dllimport)
#    endif
#else
#    define ENGINE_API
#endif

#endif
