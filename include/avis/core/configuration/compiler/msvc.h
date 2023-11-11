#ifndef CORE_CONFIGURATION_COMPILER_MSVC_H
#define CORE_CONFIGURATION_COMPILER_MSVC_H

#include "avis/core/configuration/preprocessor.h"

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

#endif
