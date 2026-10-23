#include "avis/threading/thread_priority.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace avis::threading
{
    int to_win32_priority(const thread_priority priority) noexcept
    {
        switch (priority)
        {
        case thread_priority::below_normal:
            return THREAD_PRIORITY_BELOW_NORMAL;
        case thread_priority::normal:
            return THREAD_PRIORITY_NORMAL;
        case thread_priority::above_normal:
            return THREAD_PRIORITY_ABOVE_NORMAL;
        case thread_priority::highest:
            return THREAD_PRIORITY_HIGHEST;
        }
        return THREAD_PRIORITY_NORMAL;
    }
} // namespace avis::threading
