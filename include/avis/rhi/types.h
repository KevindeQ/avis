#ifndef AVIS_RHI_TYPES_H
#define AVIS_RHI_TYPES_H

// Basic RHI value types and handles (docs/design/09-rhi.md §3.1–3.2). Free of D3D12 and <windows.h>.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace avis::rhi
{
    enum class queue_type : std::uint8_t
    {
        graphics, // DIRECT
        compute, // COMPUTE
        copy // COPY
    };

    // GPU virtual address (D3D12_GPU_VIRTUAL_ADDRESS); 0 is invalid
    struct gpu_address
    {
        std::uint64_t value = 0;

        friend constexpr bool operator==(gpu_address, gpu_address) noexcept = default;

        constexpr gpu_address offset_by(const std::uint64_t bytes) const noexcept
        {
            return { value + bytes };
        }
    };

    // Index into the shader-visible CBV/SRV/UAV heap or the sampler heap. 0 is the null descriptor of the region
    // (§9.2), so a default-constructed index samples zero instead of faulting.
    struct bindless_index
    {
        std::uint32_t value = 0;

        friend constexpr bool operator==(bindless_index, bindless_index) noexcept = default;
    };

    // Locally unique adapter id, as reported by Qt (adapterLuidLow / adapterLuidHigh) and DXGI
    struct adapter_luid
    {
        std::uint32_t low = 0;
        std::int32_t high = 0;

        friend constexpr bool operator==(adapter_luid, adapter_luid) noexcept = default;
    };

    struct subresource
    {
        std::uint16_t mip = 0;
        std::uint16_t layer = 0; // array layer; for cubes, cube index × 6 + face
    };

    struct subresource_range
    {
        static constexpr std::uint16_t all = 0xFFFF; // up to the last mip or layer

        std::uint16_t first_mip = 0;
        std::uint16_t mip_count = all;
        std::uint16_t first_layer = 0;
        std::uint16_t layer_count = all;
    };

    inline constexpr subresource_range all_subresources{};

    struct viewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minimum_depth = 0.0f;
        float maximum_depth = 1.0f;
    };

    struct rectangle
    {
        std::int32_t left = 0;
        std::int32_t top = 0;
        std::int32_t right = 0;
        std::int32_t bottom = 0;
    };

    // Depth test and comparison samplers (used by pipeline.h and resources.h)
    enum class comparison : std::uint8_t
    {
        never,
        less,
        equal,
        less_equal,
        greater,
        not_equal,
        greater_equal,
        always
    };

    namespace detail
    {
        // Reads and builds handles. Defined privately, in lib/avis/rhi/d3d12/slot_map.h (§21.2).
        struct handle_access;
    } // namespace detail

    template<class Tag>
    class handle;

    // 32-bit slot index + 32-bit generation into a slot map of the RHI (§8.2). A default-constructed handle is
    // invalid; a handle whose slot was destroyed is detected (stale generation) and rejected.
    template<class Tag>
    class handle
    {
    public:
        constexpr handle() noexcept = default;

        constexpr bool is_valid() const noexcept
        {
            return generation_ != 0;
        }

        friend constexpr bool operator==(handle, handle) noexcept = default;

    private:
        friend struct detail::handle_access;
        friend struct std::hash<handle>;

        std::uint32_t index_ = 0;
        std::uint32_t generation_ = 0;
    };

    using texture_handle = handle<struct texture_tag>;
    using buffer_handle = handle<struct buffer_tag>;
    using sampler_handle = handle<struct sampler_tag>;
    using heap_handle = handle<struct heap_tag>;
    using graphics_pipeline_handle = handle<struct graphics_pipeline_tag>;
    using compute_pipeline_handle = handle<struct compute_pipeline_tag>;
    using root_signature_handle = handle<struct root_signature_tag>; // §12.1
} // namespace avis::rhi

// For caches keyed by handle (the render graph's physical resource cache)
template<class Tag>
struct std::hash<avis::rhi::handle<Tag>>
{
    std::size_t operator()(avis::rhi::handle<Tag> value) const noexcept;
};

template<class Tag>
std::size_t std::hash<avis::rhi::handle<Tag>>::operator()(const avis::rhi::handle<Tag> value) const noexcept
{
    const std::uint64_t bits = (static_cast<std::uint64_t>(value.index_) << 32) | value.generation_;
    return std::hash<std::uint64_t>{}(bits);
}

#endif
