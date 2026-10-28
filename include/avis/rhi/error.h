#ifndef AVIS_RHI_ERROR_H
#define AVIS_RHI_ERROR_H

// RHI errors in error_domain::rhi (docs/design/09-rhi.md §3.4)

#include "avis/error.h"

#include <cstdint>
#include <format>
#include <source_location>
#include <string>
#include <string_view>

namespace avis::rhi
{
    enum class error_kind : std::int32_t
    {
        ok = 0,
        unsupported_os, // older than Windows 11 24H2 (D-05)
        unsupported_hardware, // a capability gate check failed (§4.4); the message names it
        adapter_not_found, // no adapter with the configured LUID, or no hardware adapter passed the gate
        debug_layer_unavailable, // debug layer or GPU-based validation requested but not installed
        out_of_memory, // E_OUTOFMEMORY or a D3D12MA allocation failure
        descriptor_heap_full, // persistent region, sampler, RTV or DSV heap exhausted
        transient_memory_exhausted, // a linear upload allocator, descriptor range or timestamp query set is full (§6)
        invalid_argument, // invalid description or parameter (zero size, bad format/usage combination)
        invalid_handle, // stale or default-constructed handle
        pipeline_creation_failed, // CreatePipelineState failed; the message carries the debug name and HRESULT
        device_lost, // DXGI_ERROR_DEVICE_REMOVED and friends (§4.7)
        native_error // any other failing HRESULT; the code and system message are in the message
    };

    std::string_view to_string(error_kind kind) noexcept;

    avis::error make_error(
        error_kind kind, std::string message, std::source_location location = std::source_location::current());

    // True when error is in error_domain::rhi with the given kind
    bool is_error(const avis::error& error, error_kind kind) noexcept;

    // Unrecoverable device removal (§4.7, 01 §6). Thrown by synchronous calls that touch a queue or fence after
    // the device was removed; the DRED report has already been logged on the "rhi" logger.
    class device_lost : public avis::runtime_error
    {
    public:
        using avis::runtime_error::runtime_error;
    };
} // namespace avis::rhi

// Formats an error_kind by name, for example "invalid_handle"
template<>
struct std::formatter<avis::rhi::error_kind> : std::formatter<std::string_view>
{
    template<typename FormatContext>
    auto format(const avis::rhi::error_kind kind, FormatContext& context) const
    {
        return std::formatter<std::string_view>::format(avis::rhi::to_string(kind), context);
    }
};

#endif
