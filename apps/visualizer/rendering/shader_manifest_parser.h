#ifndef AVIS_VISUALIZER_RENDERING_SHADER_MANIFEST_PARSER_H
#define AVIS_VISUALIZER_RENDERING_SHADER_MANIFEST_PARSER_H

// The Visualizer's parser for the manifest.yaml that avis_add_shaders writes (docs/design/12a-visualizer-shader-
// manifest.md, 18 §6, 13 §3). The library has no parser for the build's format (D-73).

#include "avis/error.h"
#include "avis/graphics/shader_manifest.h"

#include <cstdint>
#include <string_view>

namespace avis::visualizer::rendering
{
    // The manifest versions this parser reads: 1 (no bindings) and 2 (a `bindings` key per entry, required)
    inline constexpr std::uint32_t oldest_shader_manifest_format = 1;
    inline constexpr std::uint32_t newest_shader_manifest_format = 2;

    // Parses the text of a manifest.yaml. `source_name` prefixes error messages (normally the file path). The first
    // problem wins; errors are error_domain::parse and read
    //   "<source_name>:<line>:<column>: <key path>: <message>"
    // for example "manifest.yaml:14:12: shaders[1].stage: expected vs, ps or cs, got 'gs'".
    // The manifest is generated, so the parser is strict (12a Q1 and Q2, decided for task 11.1): unknown keys are
    // errors, every entry needs an `xxh3`, and format 2 entries need their `bindings`. Pure: no file access, no
    // logging, no global state; safe on any thread.
    result<graphics::shader_manifest>
        parse_shader_manifest(std::string_view yaml_text, std::string_view source_name = {});
} // namespace avis::visualizer::rendering

#endif
