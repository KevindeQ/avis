#ifndef AVIS_DETAIL_SAFE_ERROR_H
#define AVIS_DETAIL_SAFE_ERROR_H

// Private: noexcept error factories for code that completes operations (IO, RHI). Building an error copies or formats
// a message, which can throw; these fall back to an error without a message.
// Shared by the IO layer and the RHI (docs/design/07-io-layer.md §4.8); includes neither of them.

#include "avis/error.h"

#include <cstdint>
#include <exception>

namespace avis::detail
{
    // An error with a message, or the same error without one if copying the message throws
    inline avis::error
        safe_error(const error_domain domain, const std::int32_t code, const char* const message) noexcept
    {
        try
        {
            return avis::error{ .domain = domain, .code = code, .message = message != nullptr ? message : "" };
        }
        catch (...)
        {
            return avis::error{ .domain = domain, .code = code };
        }
    }

    // The exception being handled, as avis::error; generic_error_kind::unhandled_exception if that fails too
    inline avis::error current_exception_error() noexcept
    {
        try
        {
            return avis::make_error(std::current_exception());
        }
        catch (...)
        {
            return avis::error{ .domain = error_domain::generic,
                                .code = static_cast<std::int32_t>(generic_error_kind::unhandled_exception) };
        }
    }
} // namespace avis::detail

#endif
