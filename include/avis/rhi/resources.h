#ifndef AVIS_RHI_RESOURCES_H
#define AVIS_RHI_RESOURCES_H

// Resource descriptions, persistent views, copy footprints and the resources view (docs/design/09-rhi.md §4.3, §8.1,
// §8.3, §11.1). Free of D3D12 and <windows.h>.

#include "avis/enum_flags.h"
#include "avis/error.h"
#include "avis/rhi/barriers.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

    class render_hardware_interface;

    namespace d3d12
    {
        class device_state;
    } // namespace d3d12

    // A view (§4.3): non-owning, copyable, built from the RHI and never outliving it. Textures, buffers and samplers;
    // placed resources are created through memory (memory.h) and destroyed here like any other.
    class resources
    {
    public:
        explicit resources(render_hardware_interface& hardware_interface) noexcept;

        result<texture_handle> create_texture(const texture_description& description);
        result<buffer_handle> create_buffer(const buffer_description& description);
        result<sampler_handle> create_sampler(const sampler_description& description);

        // Deferred until the GPU is done with it (§8.2). A stale or invalid handle is a no-op and asserts in debug
        // builds.
        void destroy(texture_handle texture) noexcept;
        void destroy(buffer_handle buffer) noexcept;
        void destroy(sampler_handle sampler) noexcept;

        // debug_name is empty here. A stale handle asserts and returns a default description.
        const texture_description& description(texture_handle texture) const;
        const buffer_description& description(buffer_handle buffer) const;

        // Persistent bindless indices, created with the resource from its usage (§8.3). An index the usage didn't
        // create (or a placed resource's, §8.4) is the null descriptor, 0, and asserts in debug builds.
        bindless_index srv(texture_handle texture) const;
        bindless_index uav(texture_handle texture, std::uint32_t mip = 0) const;
        bindless_index srv(buffer_handle buffer) const;
        bindless_index uav(buffer_handle buffer) const;
        bindless_index sampler_index(sampler_handle sampler) const;

        gpu_address buffer_address(buffer_handle buffer) const;
        std::span<std::byte> mapped_bytes(buffer_handle buffer) const; // upload and readback buffers only

        // The buffer layout of one subresource for copies (§11.1); thread-safe, no GPU work
        copy_footprint copyable_footprint(texture_handle texture, subresource subresource) const;

    private:
        d3d12::device_state* state_;
    };
} // namespace avis::rhi

#endif
