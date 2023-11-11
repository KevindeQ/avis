#ifndef CORE_CONFIGURATION_BUILD_CONFIGURATION_H
#define CORE_CONFIGURATION_BUILD_CONFIGURATION_H

#include "avis/core/configuration/preprocessor.h"

// Version configuration
#define ENGINE_VERSION_MAJOR 0
#define ENGINE_VERSION_MINOR 1
#define ENGINE_VERSION_PATCH 0
#define ENGINE_FULL_VERSION_STRING \
    "v" STRINGIZE(ENGINE_VERSION_MAJOR) "." STRINGIZE(ENGINE_VERSION_MINOR) "." STRINGIZE(ENGINE_VERSION_PATCH)

#if defined(ENGINE_PLATFORM_WINDOWS)
#    include "avis/core/configuration/platform/windows.h"
#else
#    error "Unsupported compiler configured in build system."
#endif

#if defined(ENGINE_COMPILER_MSVC)
#    include "avis/core/configuration/compiler/msvc.h"
#else
#    error "Unsupported platform configured in build system."
#endif

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
