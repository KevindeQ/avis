#include "avis/rhi/error.h"

#include <utility>

namespace avis::rhi
{
    std::string_view to_string(const error_kind kind) noexcept
    {
        switch (kind)
        {
        case error_kind::ok:
            return "ok";
        case error_kind::unsupported_os:
            return "unsupported_os";
        case error_kind::unsupported_hardware:
            return "unsupported_hardware";
        case error_kind::adapter_not_found:
            return "adapter_not_found";
        case error_kind::debug_layer_unavailable:
            return "debug_layer_unavailable";
        case error_kind::out_of_memory:
            return "out_of_memory";
        case error_kind::descriptor_heap_full:
            return "descriptor_heap_full";
        case error_kind::transient_memory_exhausted:
            return "transient_memory_exhausted";
        case error_kind::invalid_argument:
            return "invalid_argument";
        case error_kind::invalid_handle:
            return "invalid_handle";
        case error_kind::pipeline_creation_failed:
            return "pipeline_creation_failed";
        case error_kind::device_lost:
            return "device_lost";
        case error_kind::native_error:
            return "native_error";
        }
        return "unknown";
    }

    avis::error make_error(const error_kind kind, std::string message, const std::source_location location)
    {
        return avis::error{
            .domain = error_domain::rhi,
            .code = static_cast<std::int32_t>(kind),
            .message = std::move(message),
            .location = location,
        };
    }

    bool is_error(const avis::error& error, const error_kind kind) noexcept
    {
        return error.domain == error_domain::rhi && error.code == static_cast<std::int32_t>(kind);
    }
} // namespace avis::rhi
