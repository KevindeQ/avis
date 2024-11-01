#ifndef BUILD_CONFIGURATION_H
#define BUILD_CONFIGURATION_H

#include "avis/preprocessor.h"

// Version configuration
#define ENGINE_VERSION_MAJOR 0
#define ENGINE_VERSION_MINOR 1
#define ENGINE_VERSION_PATCH 0
#define ENGINE_FULL_VERSION_STRING \
    "v" STRINGIZE(ENGINE_VERSION_MAJOR) "." STRINGIZE(ENGINE_VERSION_MINOR) "." STRINGIZE(ENGINE_VERSION_PATCH)

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
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <wrl/client.h>
#include <xmmintrin.h> // SSE / SSE2 intrinsics

#include <Windows.h>

// Control for exporting the engine API from its dll
#if defined(ENGINE_BUILD_SHARED)
#    if defined(ENGINE_EXPORT_API)
#        define __declspec(dllexport)
#    else
#        define __declspec(dllimport)
#    endif
#else
#    define ENGINE_API
#endif

#if (_MSC_VER >= 1930)
#    define ENGINE_COMPILER_NAME "Microsoft Visual Studio 2022"
#    define ENGINE_RUNTIME_NAME "Visual C++ 17.0.1"

#    pragma warning(disable : 4251) // Disable warnings on exporting std library types from dll
#else
#    error "Unsupported Visual C++ version."
#endif

// Macro for marking deprecated code, functions and types
#define DEPRECATED(version)                                                             \
    __declspec(deprecated("Please update your code to the new API before upgrading to " \
                          "version " STRINGIZE(version) " or newer, otherwise your "    \
                                                        "project no longer compiles."))

#define ENABLE +
#define DISABLE -
#define ENABLED(option) ((1 option 1) == 2)
#define DISABLED(option) ((1 option 1) == 0)

// Common features configuration
#define ENGINE_DOUBLEPRECISION ENABLE

// Debugging features configuration
#if defined(ENGINE_BUILD_DEBUG)
#    define ENGINE_REPORT_CRT_MEMLEAKS ENABLE
#    define ENGINE_ASSERTIONS DISABLE   // ENABLE
#elif defined(ENGINE_BUILD_PROFILE)
#    define ENGINE_REPORT_CRT_MEMLEAKS ENABLE
#    define ENGINE_ASSERTIONS DISABLE
#else
#    define ENGINE_REPORT_CRT_MEMLEAKS DISABLE
#    define ENGINE_ASSERTIONS DISABLE
#endif

// Graphics features configuration
#if defined(ENGINE_BUILD_DEBUG)
#    define ENGINE_GRAPHICS_DEBUG ENABLE
#    define ENGINE_GRAPHICS_PROFILER DISABLE
#elif defined(ENGINE_BUILD_PROFILE)
#    define ENGINE_GRAPHICS_DEBUG DISABLE
#    define ENGINE_GRAPHICS_PROFILER ENABLE
#else
#    define ENGINE_GRAPHICS_DEBUG DISABLE
#    define ENGINE_GRAPHICS_PROFILER DISABLE
#endif

#endif
