#ifndef AVIS_GRAPHICS_ERROR_H
#define AVIS_GRAPHICS_ERROR_H

// Graphics-layer errors in error_domain::graphics (docs/design/17-graphics-frames-and-transfers.md §2). RHI errors pass
// through the graphics layer unchanged, in error_domain::rhi.

#include "avis/error.h"

#include <cstdint>
#include <format>
#include <source_location>
#include <string>
#include <string_view>

namespace avis::graphics
{
    enum class error_kind : std::int32_t
    {
        ok = 0,
        upload_queue_full, // a synchronous upload didn't fit the staging ring right now
        invalid_argument, // bad destination, size or subresource data
        shader_not_found, // 18: a shader id isn't in the library
        shader_stage_mismatch, // 18: a shader used in the wrong pipeline slot
        shader_hash_mismatch, // 18: a blob doesn't match its manifest hash
        duplicate_shader_id, // 18: two blobs with the same id or id hash
        no_compatible_root_signature, // 18: no registered root signature fits the pipeline's shaders
        root_signature_incompatible, // 18: the pipeline's named root signature doesn't fit its shaders
        shader_bindings_missing // 18: a shader has no bindings and the pipeline names no root signature
    };

    std::string_view to_string(error_kind kind) noexcept;

    avis::error make_error(
        error_kind kind, std::string message, std::source_location location = std::source_location::current());

    // True when error is in error_domain::graphics with the given kind
    bool is_error(const avis::error& error, error_kind kind) noexcept;
} // namespace avis::graphics

// Formats an error_kind by name, for example "upload_queue_full"
template<>
struct std::formatter<avis::graphics::error_kind> : std::formatter<std::string_view>
{
    template<typename FormatContext>
    auto format(const avis::graphics::error_kind kind, FormatContext& context) const
    {
        return std::formatter<std::string_view>::format(avis::graphics::to_string(kind), context);
    }
};

#endif
