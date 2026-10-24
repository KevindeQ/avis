#include "avis/io/error.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <format>
#include <utility>
#include <windows.h>

namespace avis::io
{
    namespace
    {
        error_kind classify_win32(const int value) noexcept
        {
            switch (value)
            {
            case ERROR_SUCCESS:
                return error_kind::ok;
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                return error_kind::not_found;
            case ERROR_ACCESS_DENIED:
                return error_kind::permission_denied;
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:
                return error_kind::sharing_violation;
            case ERROR_FILE_EXISTS:
            case ERROR_ALREADY_EXISTS:
                return error_kind::already_exists;
            case ERROR_DISK_FULL:
            case ERROR_HANDLE_DISK_FULL:
                return error_kind::disk_full;
            case ERROR_INVALID_PARAMETER:
                return error_kind::invalid_argument;
            default:
                return error_kind::system_error;
            }
        }

        error_kind classify_errno(const std::error_code& code) noexcept
        {
            if (!code)
            {
                return error_kind::ok;
            }

            const std::errc condition = static_cast<std::errc>(code.value());
            switch (condition)
            {
            case std::errc::no_such_file_or_directory:
                return error_kind::not_found;
            case std::errc::permission_denied:
                return error_kind::permission_denied;
            case std::errc::file_exists:
                return error_kind::already_exists;
            case std::errc::no_space_on_device:
                return error_kind::disk_full;
            case std::errc::invalid_argument:
                return error_kind::invalid_argument;
            case std::errc::file_too_large:
                return error_kind::file_too_large;
            default:
                return error_kind::system_error;
            }
        }

        // The value alone is ambiguous (asio::error::eof and ERROR_FILE_NOT_FOUND are both 2), so the category decides
        error_kind classify(const std::error_code& code) noexcept
        {
            if (code.category() == std::system_category())
            {
                return classify_win32(code.value());
            }
            if (code.category() == std::generic_category())
            {
                return classify_errno(code);
            }
            return error_kind::system_error;
        }

        // System messages end with ".\r\n" on Windows; keep the text, drop the line break and trailing spaces
        std::string trimmed_message(const std::error_code& code)
        {
            std::string message = code.message();
            while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ' ||
                                        message.back() == '\t'))
            {
                message.pop_back();
            }
            return message;
        }

        std::string utf8(const std::filesystem::path& path)
        {
            const std::u8string text = path.u8string();
            return std::string{ text.begin(), text.end() };
        }
    } // namespace

    std::string_view to_string(const error_kind code) noexcept
    {
        switch (code)
        {
        case error_kind::ok:
            return "ok";
        case error_kind::not_found:
            return "not_found";
        case error_kind::permission_denied:
            return "permission_denied";
        case error_kind::sharing_violation:
            return "sharing_violation";
        case error_kind::already_exists:
            return "already_exists";
        case error_kind::invalid_argument:
            return "invalid_argument";
        case error_kind::not_open:
            return "not_open";
        case error_kind::disk_full:
            return "disk_full";
        case error_kind::file_too_large:
            return "file_too_large";
        case error_kind::short_write:
            return "short_write";
        case error_kind::mapping_failed:
            return "mapping_failed";
        case error_kind::system_error:
            return "system_error";
        }
        return "unknown";
    }

    error make_error(const error_kind code, std::string message, const std::source_location location)
    {
        return error{
            .domain = error_domain::io,
            .code = static_cast<std::int32_t>(code),
            .message = std::move(message),
            .location = location,
        };
    }

    error make_error(
        const std::error_code& code,
        const std::string_view operation,
        const std::filesystem::path& path,
        const std::source_location location)
    {
        const std::string native = std::format("({} error {})", code.category().name(), code.value());
        std::string message = path.empty() ?
                                  std::format("{}: {} {}", operation, trimmed_message(code), native) :
                                  std::format("{} '{}': {} {}", operation, utf8(path), trimmed_message(code), native);
        return make_error(classify(code), std::move(message), location);
    }

    bool is_error(const avis::error& error, const error_kind code) noexcept
    {
        return error.domain == error_domain::io && error.code == static_cast<std::int32_t>(code);
    }
} // namespace avis::io
