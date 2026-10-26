#ifndef AVIS_IO_DETAIL_SAFE_ERROR_H
#define AVIS_IO_DETAIL_SAFE_ERROR_H

// Private: noexcept versions of the IO error factories for code that completes operations, on top of the generic
// avis::detail::safe_error. make_error formats a message, which can throw; these fall back to an error without a
// message. current_exception_error is the generic one in lib/avis/detail/safe_error.h.

#include "avis/detail/safe_error.h"
#include "avis/error.h"
#include "avis/io/error.h"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace avis::io::detail
{
    inline avis::error safe_error(const error_kind kind, const char* const message) noexcept
    {
        return avis::detail::safe_error(error_domain::io, static_cast<std::int32_t>(kind), message);
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
            return avis::detail::safe_error(
                error_domain::io, static_cast<std::int32_t>(error_kind::system_error), nullptr);
        }
    }
} // namespace avis::io::detail

#endif
