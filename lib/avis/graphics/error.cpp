#include "avis/graphics/error.h"

#include <utility>

namespace avis::graphics
{
    std::string_view to_string(const error_kind kind) noexcept
    {
        switch (kind)
        {
        case error_kind::ok:
            return "ok";
        case error_kind::upload_queue_full:
            return "upload_queue_full";
        case error_kind::invalid_argument:
            return "invalid_argument";
        case error_kind::shader_not_found:
            return "shader_not_found";
        case error_kind::shader_stage_mismatch:
            return "shader_stage_mismatch";
        case error_kind::shader_hash_mismatch:
            return "shader_hash_mismatch";
        case error_kind::duplicate_shader_id:
            return "duplicate_shader_id";
        case error_kind::no_compatible_root_signature:
            return "no_compatible_root_signature";
        case error_kind::root_signature_incompatible:
            return "root_signature_incompatible";
        case error_kind::shader_bindings_missing:
            return "shader_bindings_missing";
        }
        return "unknown";
    }

    avis::error make_error(const error_kind kind, std::string message, const std::source_location location)
    {
        return avis::error{
            .domain = error_domain::graphics,
            .code = static_cast<std::int32_t>(kind),
            .message = std::move(message),
            .location = location,
        };
    }

    bool is_error(const avis::error& error, const error_kind kind) noexcept
    {
        return error.domain == error_domain::graphics && error.code == static_cast<std::int32_t>(kind);
    }
} // namespace avis::graphics
