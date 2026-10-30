#ifndef AVIS_TESTS_HELPERS_GRAPHICS_TEST_SHADERS_H
#define AVIS_TESTS_HELPERS_GRAPHICS_TEST_SHADERS_H

// The graphics test shaders of tests/shaders/graphics/ (docs/tasks/08-graphics-shaders-and-pipelines.md): their ids,
// the bindings written by hand for each, and the second root signature some of them need. The pipeline cache tests
// use the hand-written bindings; the shader build test checks them against what the build reflected.

#include "avis/graphics/shader_bindings.h"
#include "avis/graphics/shader_library.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/root_signature.h"

#include <array>
#include <optional>
#include <string_view>

namespace avis::tests::graphics_test_shaders
{
    // Ids follow avis_add_shaders: <file name>.<entry>[.<permutation, lowercase>].<stage>
    inline constexpr std::string_view binding_layout_compute = "binding_layout_test.main.cs";
    inline constexpr std::string_view draw_vertex = "pipeline_test_draw.vertex_main.vs";
    inline constexpr std::string_view draw_vertex_tinted = "pipeline_test_draw.vertex_main.tinted.vs";
    inline constexpr std::string_view draw_pixel = "pipeline_test_draw.pixel_main.ps";
    inline constexpr std::string_view draw_pixel_tinted = "pipeline_test_draw.pixel_main.tinted.ps";
    inline constexpr std::string_view buffers_compute = "pipeline_test_buffers.main.cs";

    inline constexpr std::array all_ids{
        binding_layout_compute, draw_vertex, draw_vertex_tinted, draw_pixel, draw_pixel_tinted, buffers_compute,
    };

    // What each shader binds, in the order the build writes it: by space and register, buffers by kind first.
    // Constant buffer sizes are as reflected, padded to whole 16-byte registers.
    inline std::optional<graphics::shader_bindings> expected_bindings(const std::string_view id)
    {
        using graphics::buffer_binding_kind;
        if (id == binding_layout_compute)
        {
            return graphics::shader_bindings{
                .constant_buffers = { { .shader_register = 0, .size = 16 },
                                      { .shader_register = 1, .size = 16 },
                                      { .shader_register = 2, .size = 16 } },
            };
        }
        if (id == draw_vertex || id == draw_vertex_tinted)
        {
            return graphics::shader_bindings{ .constant_buffers = { { .shader_register = 0, .size = 16 } } };
        }
        if (id == draw_pixel || id == draw_pixel_tinted)
        {
            return graphics::shader_bindings{
                .constant_buffers = { { .shader_register = 1, .size = 32 } },
                .samplers = { { .shader_register = 1 } },
            };
        }
        if (id == buffers_compute)
        {
            return graphics::shader_bindings{
                .constant_buffers = { { .shader_register = 0, .size = 16 } },
                .buffers = { { .kind = buffer_binding_kind::shader_resource, .shader_register = 0 },
                             { .kind = buffer_binding_kind::unordered_access, .shader_register = 0 } },
                .samplers = { { .shader_register = 0 } },
            };
        }
        return std::nullopt;
    }

    // The second test signature, for pipeline_test_buffers: 4 root constants at b0, a root SRV at t0, a root UAV at
    // u0 and a point-clamp static sampler at s0. The standard signature can't run that shader (no t0, no u0).
    inline constexpr std::array buffers_parameters{
        rhi::root_parameter{ .kind = rhi::root_parameter_kind::constants, .shader_register = 0, .constant_count = 4 },
        rhi::root_parameter{ .kind = rhi::root_parameter_kind::shader_resource, .shader_register = 0 },
        rhi::root_parameter{ .kind = rhi::root_parameter_kind::unordered_access, .shader_register = 0 },
    };

    inline constexpr std::array buffers_samplers{
        rhi::static_sampler{ .sampler = { .minification = rhi::filter::point,
                                          .magnification = rhi::filter::point,
                                          .mip = rhi::filter::point,
                                          .address_u = rhi::address_mode::clamp,
                                          .address_v = rhi::address_mode::clamp,
                                          .address_w = rhi::address_mode::clamp },
                             .shader_register = 0 },
    };

    inline rhi::root_signature_description buffers_description() noexcept
    {
        return { .parameters = buffers_parameters, .static_samplers = buffers_samplers, .debug_name = "buffers" };
    }
} // namespace avis::tests::graphics_test_shaders

#endif
