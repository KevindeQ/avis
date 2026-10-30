#ifndef AVIS_GRAPHICS_SHADER_MANIFEST_H
#define AVIS_GRAPHICS_SHADER_MANIFEST_H

// The build's shader manifest as a structure (docs/design/18-graphics-shaders-and-pipelines.md §3). The library offers
// no parser for the build's YAML format (D-73): the application builds this from whatever source it likes.

#include "avis/graphics/shader_bindings.h"
#include "avis/graphics/shader_library.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace avis::graphics
{
    struct shader_manifest_entry
    {
        std::string id; // "lighting.main.cs"
        shader_stage stage = shader_stage::compute;
        std::filesystem::path blob; // relative to the blob directory: "lighting.main.cs.cso"
        std::string entry; // informational: the HLSL entry point
        std::vector<std::string> defines; // informational: the compiler defines
        std::optional<std::uint64_t> content_hash; // xxh3 of the blob; checked when present
        std::optional<shader_bindings> bindings; // copied into the blob (18 §6)
    };

    struct shader_manifest
    {
        std::vector<shader_manifest_entry> entries;
    };
} // namespace avis::graphics

#endif
