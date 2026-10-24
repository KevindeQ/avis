#ifndef AVIS_IO_ERROR_H
#define AVIS_IO_ERROR_H

#include "avis/error.h"

#include <cstdint>
#include <filesystem>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>

namespace avis::io
{
    // Codes of errors in error_domain::io. Portable classification, stable for tests and callers; the native code is
    // kept in the message (docs/design/07-io-layer.md §5.1, D-67).
    enum class error_kind : std::int32_t
    {
        ok = 0,
        not_found, // ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND
        permission_denied, // ERROR_ACCESS_DENIED
        sharing_violation, // ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION
        already_exists, // ERROR_FILE_EXISTS, ERROR_ALREADY_EXISTS
        invalid_argument, // bad open flags, offset overflow, ERROR_INVALID_PARAMETER
        not_open, // operation on a closed or moved-from file
        disk_full, // ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL
        file_too_large, // read_file on a file whose size doesn't fit a std::vector<std::byte>
        short_write, // a write transferred 0 bytes without an error code
        mapping_failed, // CreateFileMappingW / MapViewOfFile
        system_error // anything not classified above; the native code is in the message
    };

    std::string_view to_string(error_kind code) noexcept;

    error make_error(
        error_kind code, std::string message, std::source_location location = std::source_location::current());

    // Classifies a native error by its category and formats
    // "<operation> '<path>': <system message> (<category> error <value>)". An empty path drops the "'<path>'" part.
    error make_error(
        const std::error_code& code,
        std::string_view operation,
        const std::filesystem::path& path,
        std::source_location location = std::source_location::current());

    // True when error is in error_domain::io with the given code
    bool is_error(const avis::error& error, error_kind code) noexcept;
} // namespace avis::io

#endif
