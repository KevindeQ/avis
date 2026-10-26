#ifndef AVIS_DETAIL_CHECK_H
#define AVIS_DETAIL_CHECK_H

// Private: checks for programming errors (docs/design/01-system-architecture.md §6.1, rule 6).
//
//     if (!AVIS_CHECK("rhi", handle_is_valid, "set_pipeline: stale pipeline handle"))
//     {
//         return;
//     }
//
// AVIS_CHECK evaluates the condition once and yields it as a bool. A failed check logs the message with the caller's
// location on the named logger: in debug builds at critical, followed by a call to the assertion handler (which
// breaks into the debugger or aborts by default); in release builds at error, and the caller goes on.
// Layers wrap it with their logger name, for example AVIS_RHI_CHECK.

#include <source_location>
#include <string_view>

namespace avis::detail
{
    using assertion_handler = void (*)(std::string_view message, std::source_location location) noexcept;

    // Installs the handler that failed checks call in debug builds and returns the previous one. nullptr installs the
    // default handler: __debugbreak() when a debugger is attached, otherwise std::abort(). Test hook: a handler that
    // records the message lets a test check an assertion without a death test.
    assertion_handler set_assertion_handler(assertion_handler handler) noexcept;

    // The failure path of AVIS_CHECK: logs, calls the handler in debug builds, and returns false
    bool check_failed(std::string_view logger, std::string_view message, std::source_location location) noexcept;
} // namespace avis::detail

#define AVIS_CHECK(logger, condition, message) \
    (static_cast<bool>(condition) || ::avis::detail::check_failed((logger), (message), std::source_location::current()))

#endif
