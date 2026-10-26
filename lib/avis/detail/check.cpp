// Checks for programming errors (lib/avis/detail/check.h)

#include "avis/detail/check.h"

#include "avis/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <atomic>
#include <cstdlib>
#include <intrin.h>
#include <windows.h>

namespace avis::detail
{
    namespace
    {
        void default_assertion_handler(std::string_view, std::source_location) noexcept
        {
            if (IsDebuggerPresent())
            {
                __debugbreak();
                return;
            }
            std::abort();
        }

        std::atomic<assertion_handler> current_handler{ &default_assertion_handler };
    } // namespace

    assertion_handler set_assertion_handler(const assertion_handler handler) noexcept
    {
        return current_handler.exchange(handler != nullptr ? handler : &default_assertion_handler);
    }

    bool check_failed(
        const std::string_view logger, const std::string_view message, const std::source_location location) noexcept
    {
        try
        {
#ifdef NDEBUG
            constexpr auto level = spdlog::level::err;
#else
            constexpr auto level = spdlog::level::critical;
#endif
            log::get(logger)->log(level, "check failed: {} ({}:{})", message, location.file_name(), location.line());
        }
        catch (...)
        {
            // Logging is best effort: a failed check must reach the handler even when the message can't be logged
        }

#ifndef NDEBUG
        current_handler.load()(message, location);
#endif
        return false;
    }
} // namespace avis::detail
