#ifndef AVIS_RHI_RESOURCES_H
#define AVIS_RHI_RESOURCES_H

// Resource descriptions, persistent views and copy footprints (docs/design/09-rhi.md §8.1, §8.3, §11.1). The
// resources view follows with the resource manager.

#include "avis/enum_flags.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace avis::rhi
{
    enum class texture_dimension : std::uint8_t
    {
        texture_1d,
        texture_2d,
        texture_3d,
        cube
    };

    enum class texture_usage : std::uint32_t
    {
        shader_resource = 1,
        unordered_access = 2,
        render_target = 4,
        depth_stencil = 8,
        shared = 16, // cross-device sharing (§13); committed, never placed
        copy_source = 32,
        copy_destination = 64
    };

    enum class buffer_usage : std::uint32_t
    {
        shader_resource = 1, // structured (structure_stride > 0) or raw ByteAddressBuffer
        unordered_access = 2,
        index = 4,
        indirect_arguments = 8,
        constant = 16, // root CBV target; size rounded up to 256
        copy_source = 32,
        copy_destination = 64
    };
} // namespace avis::rhi

ALLOW_FLAGS_FOR_ENUM(avis::rhi::texture_usage)
ALLOW_FLAGS_FOR_ENUM(avis::rhi::buffer_usage)

namespace avis::rhi
{
    enum class memory_type : std::uint8_t
    {
        gpu_only, // DEFAULT heap
        upload, // UPLOAD heap, persistently mapped (mapped_bytes)
        readback // READBACK heap, persistently mapped (mapped_bytes)
    };

    struct clear_value
    {
        std::array<float, 4> color{}; // for render targets
        float depth = 0.0f; // reverse-Z: 0 is the far plane
        std::uint8_t stencil = 0;
    };

    struct texture_description
    {
        texture_dimension dimension = texture_dimension::texture_2d;
        std::uint32_t width = 1;
        std::uint32_t height = 1;
        std::uint32_t depth_or_layers = 1; // 3D: depth; arrays: layers; cube: number of cubes (6 faces each)
        std::uint32_t mip_levels = 1; // 0 = full chain
        rhi::format format = rhi::format::rgba8_unorm;
        avis::flags<texture_usage> usage = texture_usage::shader_resource;
        std::optional<clear_value> optimized_clear;
        texture_layout initial_layout = texture_layout::common; // §10; uploads expect common
        std::string_view debug_name; // copied at creation
    };

    struct buffer_description
    {
        std::uint64_t size = 0;
        std::uint32_t structure_stride = 0; // 0 = raw / byte-address
        avis::flags<buffer_usage> usage{ avis::empty }; // flags' default constructor leaves the value uninitialised
        memory_type memory = memory_type::gpu_only;
        std::string_view debug_name; // copied at creation
    };

    enum class filter : std::uint8_t
    {
        point,
        linear,
        anisotropic
    };

    enum class address_mode : std::uint8_t
    {
        wrap,
        clamp,
        mirror,
        border
    };

    // Heap samplers (sampler_index), and the sampler state of a root signature's static samplers (§12.1)
    struct sampler_description
    {
        filter minification = filter::linear;
        filter magnification = filter::linear;
        filter mip = filter::linear;
        address_mode address_u = address_mode::wrap;
        address_mode address_v = address_mode::wrap;
        address_mode address_w = address_mode::wrap;
        std::uint32_t maximum_anisotropy = 1;
        std::optional<comparison> compare; // shadow samplers
        float mip_bias = 0.0f;
        std::array<float, 4> border_color{};
    };

    // Selects the subresources a render target or depth view covers (§8.3)
    struct texture_view
    {
        texture_handle texture;
        std::uint16_t mip = 0;
        std::uint16_t first_layer = 0;
        std::uint16_t layer_count = 1;
        rhi::format format = rhi::format::unknown; // unknown = the texture's own format
    };

    // The buffer layout of one subresource for copy_buffer_to_texture / copy_texture_to_buffer (GetCopyableFootprints1)
    struct copy_footprint
    {
        std::uint64_t offset = 0; // from the buffer offset passed to the copy; 0 for a single subresource
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t depth = 0;
        std::uint32_t row_pitch = 0; // 256-byte aligned
        std::uint32_t row_count = 0; // block rows for BC formats
        std::uint64_t row_size = 0; // meaningful bytes per row
        std::uint64_t total_size = 0; // bytes the copy reads or writes
    };
} // namespace avis::rhi

#endif
