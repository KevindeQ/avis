#ifndef AVIS_IO_DETAIL_SAFE_ERROR_H
#define AVIS_IO_DETAIL_SAFE_ERROR_H

// Private: noexcept versions of the error factories for code that completes operations. make_error formats a message,
// which can throw; these fall back to an error without a message.

#include "avis/error.h"
#include "avis/io/error.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace avis::io::detail
{
    inline avis::error safe_error(const error_kind kind, const char* const message) noexcept
    {
        try
        {
            return make_error(kind, message);
        }
        catch (...)
        {
            return avis::error{ .domain = error_domain::io, .code = static_cast<std::int32_t>(kind) };
        }
    }

    inline avis::error safe_error(
        const std::error_code& code, const std::string_view operation, const std::filesystem::path& path) noexcept
    {
        try
        {
            return make_error(code, operation, path);
        }
        catch (...)
        {
            return avis::error{ .domain = error_domain::io,
                                .code = static_cast<std::int32_t>(error_kind::system_error) };
        }
    }

    // The exception being handled, as avis::error
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
} // namespace avis::io::detail

#endif
