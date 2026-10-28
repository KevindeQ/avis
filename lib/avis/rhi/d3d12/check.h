#ifndef AVIS_RHI_D3D12_CHECK_H
#define AVIS_RHI_D3D12_CHECK_H

// HRESULT → avis::error, device-removal detection and the RHI's debug assertion (docs/design/09-rhi.md §20.2.2, §19.5)

#include "avis/detail/check.h"
#include "avis/error.h"
#include "avis/rhi/d3d12/d3d12.h"

#include <source_location>
#include <string_view>

// Debug assertion for programming errors in the RHI, on the "rhi" logger (01 §6.1 rule 6). Yields the condition.
#define AVIS_RHI_CHECK(condition, message) AVIS_CHECK("rhi", condition, message)

namespace avis::rhi::d3d12
{
    class device;

    // "<operation>: <system message> (HRESULT 0x887A0005)", classified into rhi::error_kind:
    // E_OUTOFMEMORY → out_of_memory, E_INVALIDARG → invalid_argument, DXGI_ERROR_DEVICE_REMOVED / _RESET / _HUNG
    // → device_lost, anything else → native_error. Falls back to a message-less error if formatting throws.
    avis::error make_error(
        HRESULT result,
        std::string_view operation,
        std::source_location location = std::source_location::current()) noexcept;

    // Success → {}, failure → unexpected(make_error(...))
    result<void> check(
        HRESULT result,
        std::string_view operation,
        std::source_location location = std::source_location::current()) noexcept;

    // For paths that throw on device removal (§4.7): a removal code goes to device.report_removed, which logs the
    // DRED report once and throws rhi::device_lost. Other failures return the error.
    result<void> check_device(
        device& device,
        HRESULT result,
        std::string_view operation,
        std::source_location location = std::source_location::current());

    // DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DEVICE_HUNG
    bool is_device_removed_code(HRESULT result) noexcept;

    // The error senders complete with after device removal (§20.4): device_lost, "<message>"
    avis::error device_lost_error(const char* message) noexcept;
} // namespace avis::rhi::d3d12

#endif
