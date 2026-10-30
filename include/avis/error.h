#ifndef AVIS_ERROR_H
#define AVIS_ERROR_H

#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

namespace avis
{
    enum class error_domain : std::uint16_t
    {
        generic,
        io,
        parse,
        network,
        rhi,
        scene,
        plugin,
        protocol,
        graphics
    };

    // Codes of errors in error_domain::generic
    enum class generic_error_kind : std::int32_t
    {
        ok = 0,
        invalid_configuration,
        pool_exhausted,
        pool_stopping,
        cancelled,
        already_exists,
        logic_error,
        unhandled_exception
    };

    struct error
    {
        error_domain domain = error_domain::generic;
        std::int32_t code = 0; // domain-specific code: generic_error_kind, io::error_kind, HRESULT, protocol status
        std::string message; // human-readable, already formatted
        std::source_location location = std::source_location::current();
    };

    template<typename T>
    using result = std::expected<T, error>;

    std::string_view to_string(error_domain domain) noexcept;
    std::string_view to_string(generic_error_kind code) noexcept;

    error make_error(
        generic_error_kind code, std::string message, std::source_location location = std::source_location::current());

    // Converts an exception into an error. Keeps the error of an avis::runtime_error and the code of a
    // std::system_error; any other exception becomes generic_error_kind::unhandled_exception.
    error make_error(std::exception_ptr exception, std::source_location location = std::source_location::current());

    // Exception for unrecoverable conditions, carrying an avis::error
    class runtime_error : public std::runtime_error
    {
    public:
        explicit runtime_error(avis::error error);

        const avis::error& error() const noexcept;

    private:
        avis::error error_;
    };
} // namespace avis

// Formats as "[<domain>:<code>] <message> (<file>:<line>)"
template<>
struct std::formatter<avis::error> : std::formatter<std::string_view>
{
    template<typename FormatContext>
    auto format(const avis::error& error, FormatContext& context) const
    {
        std::string_view file = error.location.file_name();
        if (const auto separator = file.find_last_of("/\\"); separator != std::string_view::npos)
        {
            file.remove_prefix(separator + 1);
        }

        const std::string text = std::format(
            "[{}:{}] {} ({}:{})",
            avis::to_string(error.domain),
            error.code,
            error.message,
            file,
            error.location.line());
        return std::formatter<std::string_view>::format(text, context);
    }
};

#endif
