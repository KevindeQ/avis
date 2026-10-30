#ifndef AVIS_GRAPHICS_BINDING_LAYOUT_H
#define AVIS_GRAPHICS_BINDING_LAYOUT_H

// The Avis standard binding model (docs/design/18-graphics-shaders-and-pipelines.md §9): the standard root signature
// and typed helpers over the RHI's generic root calls, for pipelines on that signature. bindless.hlsli mirrors these
// constants.

#include "avis/rhi/command_list.h"
#include "avis/rhi/root_signature.h"
#include "avis/rhi/types.h"

#include <cstdint>
#include <type_traits>

namespace avis::graphics::binding_layout
{
    inline constexpr std::uint32_t draw_constants_parameter = 0; // 32-bit constants, b0 space0
    inline constexpr std::uint32_t draw_constant_count = 16;
    inline constexpr std::uint32_t pass_constants_parameter = 1; // root CBV, b1 space0
    inline constexpr std::uint32_t view_constants_parameter = 2; // root CBV, b2 space0
    inline constexpr std::uint32_t draw_constants_register = 0;
    inline constexpr std::uint32_t pass_constants_register = 1;
    inline constexpr std::uint32_t view_constants_register = 2;

    // Register s<n>, space0
    enum class static_sampler : std::uint32_t
    {
        point_clamp = 0,
        linear_clamp = 1,
        linear_wrap = 2,
        anisotropic_wrap = 3, // 16x
        shadow_comparison = 4, // linear, GREATER_EQUAL (reverse-Z), clamp
        point_wrap = 5
    };

    inline constexpr std::uint32_t static_sampler_count = 6;

    // The standard description, for root_signature_registry::add or rhi::pipelines::create_root_signature: draw
    // constants, pass and view constants, the six static samplers, all visible to every stage. Its spans point to
    // static storage, valid for the program's lifetime.
    rhi::root_signature_description standard_description() noexcept;

    // Sets the draw constants (b0). They take a recorder, the type passes get from their context (D-92); a command_list
    // is one too. Constants that don't fit the 16 root constants don't satisfy the constraint, so the size is checked
    // at compile time.
    template<class Constants, rhi::queue_type Queue>
        requires(
            Queue != rhi::queue_type::copy && std::is_trivially_copyable_v<Constants> && sizeof(Constants) % 4 == 0 &&
            sizeof(Constants) <= draw_constant_count * 4)
    void push_draw_constants(rhi::command_recorder<Queue>& recorder, const Constants& constants)
    {
        recorder.set_root_constants(draw_constants_parameter, constants);
    }

    // Binds the pass (b1) and view (b2) constants: a root CBV address, typically from frame memory. Defined in
    // binding_layout.cpp for the graphics and compute queues (copy lists bind no root arguments).
    template<rhi::queue_type Queue>
        requires(Queue != rhi::queue_type::copy)
    void set_pass_constants(rhi::command_recorder<Queue>& recorder, rhi::gpu_address address);

    template<rhi::queue_type Queue>
        requires(Queue != rhi::queue_type::copy)
    void set_view_constants(rhi::command_recorder<Queue>& recorder, rhi::gpu_address address);

    extern template void set_pass_constants<rhi::queue_type::graphics>(rhi::graphics_recorder&, rhi::gpu_address);
    extern template void set_pass_constants<rhi::queue_type::compute>(rhi::compute_recorder&, rhi::gpu_address);
    extern template void set_view_constants<rhi::queue_type::graphics>(rhi::graphics_recorder&, rhi::gpu_address);
    extern template void set_view_constants<rhi::queue_type::compute>(rhi::compute_recorder&, rhi::gpu_address);
} // namespace avis::graphics::binding_layout

#endif
