#ifndef AVIS_RHI_ROOT_SIGNATURE_H
#define AVIS_RHI_ROOT_SIGNATURE_H

// Root signature descriptions (docs/design/09-rhi.md §12.1). Bindless is mandatory: there are no descriptor tables,
// and every signature indexes the heaps directly.

#include "avis/rhi/resources.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace avis::rhi
{
    enum class root_parameter_kind : std::uint8_t
    {
        constants, // 32-bit root constants at b<register>
        constant_buffer, // root CBV at b<register>: a gpu_address
        shader_resource, // root SRV at t<register>: a raw or structured buffer's gpu_address
        unordered_access // root UAV at u<register>: a raw or structured buffer's gpu_address
    };

    // Which shader stages see a parameter or static sampler (D3D12_SHADER_VISIBILITY). Compute shaders only see `all`.
    enum class shader_visibility : std::uint8_t
    {
        all,
        vertex,
        pixel
    };

    struct root_parameter
    {
        root_parameter_kind kind = root_parameter_kind::constants;
        std::uint32_t shader_register = 0;
        std::uint32_t register_space = 0;
        std::uint32_t constant_count = 0; // constants only: number of 32-bit values
        shader_visibility visibility = shader_visibility::all;
    };

    // A sampler baked into the signature: no heap slot, register s<register>
    struct static_sampler
    {
        sampler_description sampler; // border_color must be transparent black, opaque black or opaque white
        std::uint32_t shader_register = 0;
        std::uint32_t register_space = 0;
        shader_visibility visibility = shader_visibility::all;
    };

    // Parameter i of the description is root parameter i. Only read during create_root_signature.
    struct root_signature_description
    {
        std::span<const root_parameter> parameters;
        std::span<const static_sampler> static_samplers;
        std::string_view debug_name; // copied at creation
    };
} // namespace avis::rhi

#endif
