#ifndef CORE_ASSERTIONS_H
#define CORE_ASSERTIONS_H

#include "avis/core/helper_functions.h"

#if ENABLED(ENGINE_ASSERTIONS)

// TODO(kdequ): Add user-configurable logging
#    define Verify(expression, message)      \
        {                                    \
            if (Debug::IsDebuggerAttached()) \
            {                                \
                if (!(expression))           \
                    Debug::Break();          \
            }                                \
            else                             \
                assert((expression));        \
        }
#    define VerifySuccess(expression, message) \
        {                                      \
            if (Debug::IsDebuggerAttached())   \
            {                                  \
                if (FAILED((expression)))      \
                    Debug::Break();            \
            }                                  \
            else                               \
                assert(!(expression));         \
        }

#else
#    define Verify(expression, message)
#    define VerifySuccess(expression, message) \
        {                                      \
            ThrowIfFailed(expression);         \
        }
#endif

#endif
