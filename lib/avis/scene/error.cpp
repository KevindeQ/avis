#include "avis/scene/error.h"

#include <utility>

namespace avis::scene
{
    std::string_view to_string(const error_kind kind) noexcept
    {
        switch (kind)
        {
        case error_kind::ok:
            return "ok";
        case error_kind::duplicate_id:
            return "duplicate_id";
        case error_kind::invalid_parent:
            return "invalid_parent";
        case error_kind::cycle:
            return "cycle";
        case error_kind::invalid_mesh:
            return "invalid_mesh";
        case error_kind::invalid_material:
            return "invalid_material";
        case error_kind::gltf_invalid:
            return "gltf_invalid";
        case error_kind::gltf_unsupported:
            return "gltf_unsupported";
        case error_kind::image_decode_failed:
            return "image_decode_failed";
        }
        return "unknown";
    }

    avis::error make_error(const error_kind kind, std::string message, const std::source_location location)
    {
        return avis::error{
            .domain = error_domain::scene,
            .code = static_cast<std::int32_t>(kind),
            .message = std::move(message),
            .location = location,
        };
    }

    bool is_error(const avis::error& error, const error_kind kind) noexcept
    {
        return error.domain == error_domain::scene && error.code == static_cast<std::int32_t>(kind);
    }
} // namespace avis::scene
