#ifndef AVIS_GRAPHICS_SHADER_BINDINGS_H
#define AVIS_GRAPHICS_SHADER_BINDINGS_H

// What one shader binds by register, for root signature matching (docs/design/18-graphics-shaders-and-pipelines.md
// §5). Filled from the build's reflection data (18 §6) or by hand; independent of where it comes from.

#include <cstdint>
#include <vector>

namespace avis::graphics
{
    // A cbuffer or ConstantBuffer<T> at b<register>
    struct constant_buffer_binding
    {
        std::uint32_t shader_register = 0;
        std::uint32_t register_space = 0;
        std::uint32_t size = 0; // bytes, as laid out by HLSL packing

        friend bool operator==(const constant_buffer_binding&, const constant_buffer_binding&) = default;
    };

    enum class buffer_binding_kind : std::uint8_t
    {
        shader_resource,
        unordered_access
    };

    // A buffer bound by register (t<n> / u<n>), not through the descriptor heap
    struct buffer_binding
    {
        buffer_binding_kind kind = buffer_binding_kind::shader_resource;
        std::uint32_t shader_register = 0;
        std::uint32_t register_space = 0;

        friend bool operator==(const buffer_binding&, const buffer_binding&) = default;
    };

    // A sampler bound by register (s<n>), not through the descriptor heap
    struct sampler_binding
    {
        std::uint32_t shader_register = 0;
        std::uint32_t register_space = 0;

        friend bool operator==(const sampler_binding&, const sampler_binding&) = default;
    };

    struct shader_bindings
    {
        std::vector<constant_buffer_binding> constant_buffers;
        std::vector<buffer_binding> buffers;
        std::vector<sampler_binding> samplers;
        // Textures or typed buffers bound by register can't be matched (no descriptor tables, 09 §12.1); the source
        // reports them as an error instead of putting them here.

        friend bool operator==(const shader_bindings&, const shader_bindings&) = default;
    };
} // namespace avis::graphics

#endif
