#include "avis/error.h"

#include <system_error>
#include <utility>

namespace avis
{
    std::string_view to_string(const error_domain domain) noexcept
    {
        switch (domain)
        {
        case error_domain::generic:
            return "generic";
        case error_domain::io:
            return "io";
        case error_domain::parse:
            return "parse";
        case error_domain::network:
            return "network";
        case error_domain::rhi:
            return "rhi";
        case error_domain::scene:
            return "scene";
        case error_domain::plugin:
            return "plugin";
        case error_domain::protocol:
            return "protocol";
        case error_domain::graphics:
            return "graphics";
        }
        return "unknown";
    }

    std::string_view to_string(const generic_error_kind code) noexcept
    {
        switch (code)
        {
        case generic_error_kind::ok:
            return "ok";
        case generic_error_kind::invalid_configuration:
            return "invalid_configuration";
        case generic_error_kind::pool_exhausted:
            return "pool_exhausted";
        case generic_error_kind::pool_stopping:
            return "pool_stopping";
        case generic_error_kind::cancelled:
            return "cancelled";
        case generic_error_kind::already_exists:
            return "already_exists";
        case generic_error_kind::logic_error:
            return "logic_error";
        case generic_error_kind::unhandled_exception:
            return "unhandled_exception";
        }
        return "unknown";
    }

    error make_error(const generic_error_kind code, std::string message, const std::source_location location)
    {
        return error{
            .domain = error_domain::generic,
            .code = static_cast<std::int32_t>(code),
            .message = std::move(message),
            .location = location,
        };
    }

    error make_error(const std::exception_ptr exception, const std::source_location location)
    {
        if (!exception)
        {
            return make_error(generic_error_kind::logic_error, "make_error called without an exception", location);
        }

        try
        {
            std::rethrow_exception(exception);
        }
        catch (const runtime_error& runtime_exception)
        {
            return runtime_exception.error();
        }
        catch (const std::system_error& system_exception)
        {
            return error{
                .domain = error_domain::generic,
                .code = system_exception.code().value(),
                .message = system_exception.what(),
                .location = location,
            };
        }
        catch (const std::exception& standard_exception)
        {
            return make_error(generic_error_kind::unhandled_exception, standard_exception.what(), location);
        }
        catch (...)
        {
            return make_error(generic_error_kind::unhandled_exception, "unknown exception", location);
        }
    }

    runtime_error::runtime_error(avis::error error) : std::runtime_error{ error.message }, error_{ std::move(error) } {}

    const avis::error& runtime_error::error() const noexcept
    {
        return error_;
    }
} // namespace avis
