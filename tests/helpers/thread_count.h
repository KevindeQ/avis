#ifndef AVIS_TESTS_HELPERS_THREAD_COUNT_H
#define AVIS_TESTS_HELPERS_THREAD_COUNT_H

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
// clang-format off: tlhelp32.h needs windows.h first
#include <windows.h>
#include <tlhelp32.h>
// clang-format on

#include <cstddef>
#include <stdexcept>

namespace avis::tests
{
    // Number of threads currently alive in this process, for the "no leaked threads" checks
    inline std::size_t current_process_thread_count()
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error{ "CreateToolhelp32Snapshot failed" };
        }

        const DWORD process_id = GetCurrentProcessId();
        std::size_t count = 0;

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL found = Thread32First(snapshot, &entry); found; found = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID == process_id)
            {
                ++count;
            }
        }

        CloseHandle(snapshot);
        return count;
    }
} // namespace avis::tests

#endif
