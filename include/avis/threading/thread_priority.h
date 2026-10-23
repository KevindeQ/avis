#ifndef AVIS_THREADING_THREAD_PRIORITY_H
#define AVIS_THREADING_THREAD_PRIORITY_H

#include <cstdint>

namespace avis::threading
{
    // OS scheduling priority of a long-running pool thread
    enum class thread_priority : std::int8_t
    {
        below_normal = -1,
        normal = 0,
        above_normal = 1,
        highest = 2
    };

    // The matching Win32 THREAD_PRIORITY_* value for SetThreadPriority. Returned as int, so this header needs no
    // <windows.h>.
    int to_win32_priority(thread_priority priority) noexcept;
} // namespace avis::threading

#endif
