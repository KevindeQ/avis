#ifndef AVIS_SCENE_ERROR_H
#define AVIS_SCENE_ERROR_H

// Scene errors in error_domain::scene (docs/design/08-scene-management.md §9.1). IO errors from loading pass through
// unchanged, in error_domain::io. Programming errors (stale handles, wrong thread) are debug checks, not errors.

#include "avis/error.h"

#include <cstdint>
#include <format>
#include <source_location>
#include <string>
#include <string_view>

namespace avis::scene
{
    enum class error_kind : std::int32_t
    {
        ok = 0,
        duplicate_id, // create_entity with an identity id that already exists
        invalid_parent, // the parent isn't alive, or has no node
        cycle, // set_parent would make an entity its own ancestor
        invalid_mesh, // mesh_data with indices out of range, mismatched streams or empty primitives
        invalid_material, // a texture handle that isn't alive
        gltf_invalid, // fastgltf rejected the file; the message has its path and fastgltf's reason
        gltf_unsupported, // a required feature outside 08 §5.1 (skinning, morph targets, required extensions)
        image_decode_failed // WIC couldn't decode a texture; names the image and the file
    };

    std::string_view to_string(error_kind kind) noexcept;

    avis::error make_error(
        error_kind kind, std::string message, std::source_location location = std::source_location::current());

    // True when error is in error_domain::scene with the given kind
    bool is_error(const avis::error& error, error_kind kind) noexcept;
} // namespace avis::scene

// Formats an error_kind by name, for example "duplicate_id"
template<>
struct std::formatter<avis::scene::error_kind> : std::formatter<std::string_view>
{
    template<typename FormatContext>
    auto format(const avis::scene::error_kind kind, FormatContext& context) const
    {
        return std::formatter<std::string_view>::format(avis::scene::to_string(kind), context);
    }
};

#endif
