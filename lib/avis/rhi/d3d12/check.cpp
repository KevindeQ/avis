#include "avis/rhi/d3d12/check.h"

#include "avis/detail/safe_error.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/error.h"

#include <cstdint>
#include <format>
#include <string>
#include <system_error>

namespace avis::rhi::d3d12
{
    namespace
    {
        error_kind classify(const HRESULT result) noexcept
        {
            if (is_device_removed_code(result))
            {
                return error_kind::device_lost;
            }
            switch (result)
            {
            case S_OK:
                return error_kind::ok;
            case E_OUTOFMEMORY:
                return error_kind::out_of_memory;
            case E_INVALIDARG:
                return error_kind::invalid_argument;
            default:
                return error_kind::native_error;
            }
        }

        // System messages end with ".\r\n"; keep the text, drop the line break and trailing spaces
        std::string system_message(const HRESULT result)
        {
            std::string message = std::system_category().message(static_cast<int>(result));
            while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
            {
                message.pop_back();
            }
            return message;
        }
    } // namespace

    avis::error
        make_error(const HRESULT result, const std::string_view operation, const std::source_location location) noexcept
    {
        const error_kind kind = classify(result);
        try
        {
            return rhi::make_error(
                kind,
                std::format(
                    "{}: {} (HRESULT 0x{:08X})", operation, system_message(result), static_cast<std::uint32_t>(result)),
                location);
        }
        catch (...)
        {
            avis::error error = avis::detail::safe_error(error_domain::rhi, static_cast<std::int32_t>(kind), nullptr);
            error.location = location;
            return error;
        }
    }

    result<void>
        check(const HRESULT result, const std::string_view operation, const std::source_location location) noexcept
    {
        if (SUCCEEDED(result))
        {
            return {};
        }
        return std::unexpected(make_error(result, operation, location));
    }

    result<void> check_device(
        device& device, const HRESULT result, const std::string_view operation, const std::source_location location)
    {
        if (is_device_removed_code(result))
        {
            device.report_removed(operation);
        }
        return check(result, operation, location);
    }

    bool is_device_removed_code(const HRESULT result) noexcept
    {
        return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
               result == DXGI_ERROR_DEVICE_HUNG;
    }

    avis::error device_lost_error(const char* const message) noexcept
    {
        return avis::detail::safe_error(error_domain::rhi, static_cast<std::int32_t>(error_kind::device_lost), message);
    }
} // namespace avis::rhi::d3d12
