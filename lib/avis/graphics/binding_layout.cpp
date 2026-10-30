#include "avis/graphics/binding_layout.h"

#include "avis/rhi/resources.h"

#include <array>

namespace avis::graphics::binding_layout
{
    namespace
    {
        constexpr std::array standard_parameters{
            rhi::root_parameter{ .kind = rhi::root_parameter_kind::constants,
                                 .shader_register = draw_constants_register,
                                 .register_space = 0,
                                 .constant_count = draw_constant_count,
                                 .visibility = rhi::shader_visibility::all },
            rhi::root_parameter{ .kind = rhi::root_parameter_kind::constant_buffer,
                                 .shader_register = pass_constants_register,
                                 .register_space = 0,
                                 .visibility = rhi::shader_visibility::all },
            rhi::root_parameter{ .kind = rhi::root_parameter_kind::constant_buffer,
                                 .shader_register = view_constants_register,
                                 .register_space = 0,
                                 .visibility = rhi::shader_visibility::all },
        };
        static_assert(standard_parameters[draw_constants_parameter].shader_register == draw_constants_register);
        static_assert(standard_parameters[pass_constants_parameter].shader_register == pass_constants_register);
        static_assert(standard_parameters[view_constants_parameter].shader_register == view_constants_register);

        constexpr rhi::sampler_description sampler_state(
            const rhi::filter filter, const rhi::address_mode address, const std::uint32_t maximum_anisotropy = 1)
        {
            return { .minification = filter,
                     .magnification = filter,
                     .mip = filter == rhi::filter::anisotropic ? rhi::filter::linear : filter,
                     .address_u = address,
                     .address_v = address,
                     .address_w = address,
                     .maximum_anisotropy = maximum_anisotropy };
        }

        constexpr rhi::static_sampler at(const static_sampler slot, const rhi::sampler_description& state)
        {
            return { .sampler = state,
                     .shader_register = static_cast<std::uint32_t>(slot),
                     .register_space = 0,
                     .visibility = rhi::shader_visibility::all };
        }

        // Linear filtering of the comparison result (PCF), point between mips; clamp, so samples outside the map
        // repeat its edge. GREATER_EQUAL because depth is reverse-Z (near = 1).
        constexpr rhi::sampler_description shadow_state()
        {
            rhi::sampler_description state = sampler_state(rhi::filter::linear, rhi::address_mode::clamp);
            state.mip = rhi::filter::point;
            state.compare = rhi::comparison::greater_equal;
            return state;
        }

        // In register order, so static_samplers[n] is s<n>
        constexpr std::array standard_samplers{
            at(static_sampler::point_clamp, sampler_state(rhi::filter::point, rhi::address_mode::clamp)),
            at(static_sampler::linear_clamp, sampler_state(rhi::filter::linear, rhi::address_mode::clamp)),
            at(static_sampler::linear_wrap, sampler_state(rhi::filter::linear, rhi::address_mode::wrap)),
            at(static_sampler::anisotropic_wrap, sampler_state(rhi::filter::anisotropic, rhi::address_mode::wrap, 16)),
            at(static_sampler::shadow_comparison, shadow_state()),
            at(static_sampler::point_wrap, sampler_state(rhi::filter::point, rhi::address_mode::wrap)),
        };
        static_assert(standard_samplers.size() == static_sampler_count);
    } // namespace

    rhi::root_signature_description standard_description() noexcept
    {
        return { .parameters = standard_parameters,
                 .static_samplers = standard_samplers,
                 .debug_name = "avis standard" };
    }

    template<rhi::queue_type Queue>
        requires(Queue != rhi::queue_type::copy)
    void set_pass_constants(rhi::command_recorder<Queue>& recorder, const rhi::gpu_address address)
    {
        recorder.set_root_constant_buffer(pass_constants_parameter, address);
    }

    template<rhi::queue_type Queue>
        requires(Queue != rhi::queue_type::copy)
    void set_view_constants(rhi::command_recorder<Queue>& recorder, const rhi::gpu_address address)
    {
        recorder.set_root_constant_buffer(view_constants_parameter, address);
    }

    template void set_pass_constants<rhi::queue_type::graphics>(rhi::graphics_recorder&, rhi::gpu_address);
    template void set_pass_constants<rhi::queue_type::compute>(rhi::compute_recorder&, rhi::gpu_address);
    template void set_view_constants<rhi::queue_type::graphics>(rhi::graphics_recorder&, rhi::gpu_address);
    template void set_view_constants<rhi::queue_type::compute>(rhi::compute_recorder&, rhi::gpu_address);
} // namespace avis::graphics::binding_layout
