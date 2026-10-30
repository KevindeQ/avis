#ifndef AVIS_LIB_GRAPHICS_SHADER_LIBRARY_INTERNAL_H
#define AVIS_LIB_GRAPHICS_SHADER_LIBRARY_INTERNAL_H

// Test hooks of the shader library (docs/tasks/08-graphics-shaders-and-pipelines.md, open point "Hash collision
// test"): a 64-bit FNV-1a collision between two readable ids isn't practical to find, so tests pass the hashes in.

#include "avis/graphics/shader_library.h"

#include <cstddef>
#include <optional>
#include <span>

namespace avis::graphics::detail
{
    // The shader kind in a DXIL container's program header, or nullopt when the bytes aren't a DXIL container with a
    // vertex, pixel or compute program. Used by create()'s debug check.
    std::optional<shader_stage> dxil_shader_stage(std::span<const std::byte> bytecode) noexcept;
} // namespace avis::graphics::detail

#endif
