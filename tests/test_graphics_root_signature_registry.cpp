// Root signature matching and the registry (docs/tasks/08-graphics-shaders-and-pipelines.md §8.5)

#include "helpers/recorded_assertions.h"
#include "helpers/rhi_fixture.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/error.h"
#include "avis/graphics/root_signature_matching.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_bindings.h"
#include "avis/graphics/shader_library.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/root_signature.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace avis;
using graphics::shader_stage;
using stage_bindings = graphics::root_signature_registry::stage_bindings;

namespace
{
    rhi::root_parameter constants(
        const std::uint32_t shader_register,
        const std::uint32_t count,
        const rhi::shader_visibility visibility = rhi::shader_visibility::all)
    {
        return { .kind = rhi::root_parameter_kind::constants,
                 .shader_register = shader_register,
                 .register_space = 0,
                 .constant_count = count,
                 .visibility = visibility };
    }

    rhi::root_parameter root(
        const rhi::root_parameter_kind kind,
        const std::uint32_t shader_register,
        const rhi::shader_visibility visibility = rhi::shader_visibility::all)
    {
        return { .kind = kind, .shader_register = shader_register, .register_space = 0, .visibility = visibility };
    }

    graphics::shader_bindings
        constant_buffer(const std::uint32_t shader_register, const std::uint32_t size, const std::uint32_t space = 0)
    {
        return { .constant_buffers = {
                     { .shader_register = shader_register, .register_space = space, .size = size } } };
    }

    graphics::shader_bindings buffer(const graphics::buffer_binding_kind kind, const std::uint32_t shader_register)
    {
        return { .buffers = { { .kind = kind, .shader_register = shader_register, .register_space = 0 } } };
    }

    graphics::shader_bindings sampler(const std::uint32_t shader_register)
    {
        return { .samplers = { { .shader_register = shader_register, .register_space = 0 } } };
    }

    std::optional<std::string> mismatch_of(
        const std::span<const rhi::root_parameter> parameters,
        const shader_stage stage,
        const graphics::shader_bindings& bindings,
        const std::span<const rhi::static_sampler> samplers = {})
    {
        const std::array stages{ stage_bindings{ .stage = stage, .bindings = &bindings } };
        return graphics::detail::first_mismatch({ .parameters = parameters, .static_samplers = samplers }, stages);
    }

    bool contains(const std::string& text, const std::string_view part)
    {
        return text.find(part) != std::string::npos;
    }
} // namespace

TEST_CASE("first_mismatch: constant buffers", "[graphics][unit]")
{
    const std::array root_constants{ constants(0, 16) };
    const std::array root_constant_buffer{ root(rhi::root_parameter_kind::constant_buffer, 0) };

    SECTION("within the root constants")
    {
        CHECK_FALSE(mismatch_of(root_constants, shader_stage::vertex, constant_buffer(0, 64)).has_value());
        CHECK_FALSE(mismatch_of(root_constants, shader_stage::compute, constant_buffer(0, 4)).has_value());
    }

    SECTION("too large for root constants, fine for a root CBV")
    {
        CHECK(
            mismatch_of(root_constants, shader_stage::vertex, constant_buffer(0, 96)) ==
            "vertex shader b0 space0 (96 bytes): root constants hold 64");
        CHECK_FALSE(mismatch_of(root_constant_buffer, shader_stage::vertex, constant_buffer(0, 96)).has_value());
        CHECK_FALSE(mismatch_of(root_constant_buffer, shader_stage::vertex, constant_buffer(0, 65536)).has_value());
    }

    SECTION("another register or space")
    {
        CHECK(
            mismatch_of(root_constants, shader_stage::compute, constant_buffer(1, 16)) ==
            "compute shader b1 space0 (16 bytes): no root parameter");
        CHECK(
            mismatch_of(root_constants, shader_stage::compute, constant_buffer(0, 16, 1)) ==
            "compute shader b0 space1 (16 bytes): no root parameter");
    }

    SECTION("a root SRV at the same number isn't a constant buffer")
    {
        const std::array parameters{ root(rhi::root_parameter_kind::shader_resource, 0) };
        CHECK(
            mismatch_of(parameters, shader_stage::pixel, constant_buffer(0, 16)) ==
            "pixel shader b0 space0 (16 bytes): no root parameter");
    }
}

TEST_CASE("first_mismatch: visibility per stage", "[graphics][unit]")
{
    SECTION("a pixel shader's binding provided only by a vertex-only parameter")
    {
        const std::array parameters{ root(
            rhi::root_parameter_kind::constant_buffer, 3, rhi::shader_visibility::vertex) };
        CHECK(
            mismatch_of(parameters, shader_stage::pixel, constant_buffer(3, 64)) ==
            "pixel shader b3 space0 (64 bytes) not visible");
        CHECK_FALSE(mismatch_of(parameters, shader_stage::vertex, constant_buffer(3, 64)).has_value());
    }

    SECTION("a vertex b0 and a pixel b0 matched by two stage-limited parameters")
    {
        const std::array parameters{
            constants(0, 4, rhi::shader_visibility::vertex),
            root(rhi::root_parameter_kind::constant_buffer, 0, rhi::shader_visibility::pixel),
        };
        const graphics::shader_bindings vertex = constant_buffer(0, 16);
        const graphics::shader_bindings pixel = constant_buffer(0, 256); // too large for the vertex-only constants
        const std::array stages{
            stage_bindings{ .stage = shader_stage::vertex, .bindings = &vertex },
            stage_bindings{ .stage = shader_stage::pixel, .bindings = &pixel },
        };
        CHECK_FALSE(graphics::detail::first_mismatch({ .parameters = parameters }, stages).has_value());

        // The same register in the vertex shader, too large for the vertex-only constants
        const graphics::shader_bindings large_vertex = constant_buffer(0, 256);
        const std::array large_stages{
            stage_bindings{ .stage = shader_stage::vertex, .bindings = &large_vertex },
            stage_bindings{ .stage = shader_stage::pixel, .bindings = &pixel },
        };
        CHECK(
            graphics::detail::first_mismatch({ .parameters = parameters }, large_stages) ==
            "vertex shader b0 space0 (256 bytes): root constants hold 16");
    }

    SECTION("a compute shader's binding provided only by a stage-limited parameter")
    {
        const std::array vertex_only{ constants(0, 16, rhi::shader_visibility::vertex) };
        const std::array pixel_only{ root(
            rhi::root_parameter_kind::unordered_access, 0, rhi::shader_visibility::pixel) };
        CHECK(
            mismatch_of(vertex_only, shader_stage::compute, constant_buffer(0, 16)) ==
            "compute shader b0 space0 (16 bytes) not visible");
        CHECK(
            mismatch_of(
                pixel_only, shader_stage::compute, buffer(graphics::buffer_binding_kind::unordered_access, 0)) ==
            "compute shader u0 space0 not visible");
    }

    SECTION("static samplers are visible per stage too")
    {
        const std::array samplers{ rhi::static_sampler{ .shader_register = 4,
                                                        .visibility = rhi::shader_visibility::pixel } };
        CHECK_FALSE(mismatch_of({}, shader_stage::pixel, sampler(4), samplers).has_value());
        CHECK(mismatch_of({}, shader_stage::compute, sampler(4), samplers) == "compute shader s4 space0 not visible");
    }
}

TEST_CASE("first_mismatch: buffers and samplers by register", "[graphics][unit]")
{
    using enum graphics::buffer_binding_kind;
    const std::array parameters{
        root(rhi::root_parameter_kind::shader_resource, 2),
        root(rhi::root_parameter_kind::unordered_access, 1),
    };

    SECTION("a root SRV or UAV of the right kind")
    {
        CHECK_FALSE(mismatch_of(parameters, shader_stage::compute, buffer(shader_resource, 2)).has_value());
        CHECK_FALSE(mismatch_of(parameters, shader_stage::compute, buffer(unordered_access, 1)).has_value());
    }

    SECTION("a root SRV or UAV of the wrong kind")
    {
        CHECK(
            mismatch_of(parameters, shader_stage::compute, buffer(unordered_access, 2)) ==
            "compute shader u2 space0: no root unordered access");
        CHECK(
            mismatch_of(parameters, shader_stage::compute, buffer(shader_resource, 1)) ==
            "compute shader t1 space0: no root shader resource");
    }

    SECTION("a missing sampler register")
    {
        const rhi::root_signature_description standard = graphics::binding_layout::standard_description();
        CHECK_FALSE(mismatch_of({}, shader_stage::pixel, sampler(5), standard.static_samplers).has_value());
        CHECK(
            mismatch_of({}, shader_stage::compute, sampler(6), standard.static_samplers) ==
            "compute shader s6 space0: no static sampler");
        CHECK(
            mismatch_of(parameters, shader_stage::compute, sampler(4)) ==
            "compute shader s4 space0: no static sampler");
    }
}

TEST_CASE("first_mismatch: what shaders don't use doesn't matter", "[graphics][unit]")
{
    const rhi::root_signature_description standard = graphics::binding_layout::standard_description();
    const graphics::shader_bindings empty;

    SECTION("unused parameters are fine")
    {
        CHECK_FALSE(
            mismatch_of(standard.parameters, shader_stage::pixel, constant_buffer(2, 256), standard.static_samplers)
                .has_value());
    }

    SECTION("empty bindings match anything")
    {
        CHECK_FALSE(mismatch_of({}, shader_stage::vertex, empty).has_value());
        CHECK_FALSE(
            mismatch_of(standard.parameters, shader_stage::compute, empty, standard.static_samplers).has_value());
    }

    SECTION("no stages match anything")
    {
        CHECK_FALSE(graphics::detail::first_mismatch({}, {}).has_value());
    }

    SECTION("the first failing stage is reported")
    {
        const graphics::shader_bindings fine = constant_buffer(0, 64);
        const graphics::shader_bindings missing = sampler(9);
        const std::array stages{
            stage_bindings{ .stage = shader_stage::vertex, .bindings = &fine },
            stage_bindings{ .stage = shader_stage::pixel, .bindings = &missing },
        };
        CHECK(graphics::detail::first_mismatch(standard, stages) == "pixel shader s9 space0: no static sampler");
    }
}

#ifndef NDEBUG
TEST_CASE("first_mismatch: a stage without bindings is the caller's error", "[graphics][unit]")
{
    tests::recorded_assertions assertions;
    const std::array stages{ stage_bindings{ .stage = shader_stage::pixel, .bindings = nullptr } };

    CHECK(graphics::detail::first_mismatch({}, stages) == "pixel shader: no bindings");
    CHECK(assertions.count() == 1);
}
#endif

TEST_CASE("root_signature_registry picks the first compatible candidate", "[graphics]")
{
    tests::test_rhi hardware_interface;
    graphics::root_signature_registry registry{ rhi::pipelines{ *hardware_interface } };

    const auto standard = registry.add("standard", graphics::binding_layout::standard_description());
    REQUIRE(standard.has_value());

    // The second model: 4 root constants at b0 and a root UAV at u0. Its arrays are gone before it's used, so the
    // registry must hold its own copy.
    std::optional<rhi::root_signature_handle> uav_model;
    {
        std::vector<rhi::root_parameter> parameters{ constants(0, 4),
                                                     root(rhi::root_parameter_kind::unordered_access, 0) };
        uav_model = registry.add("uav model", { .parameters = parameters }).value();
        parameters.assign(parameters.size(), constants(7, 1));
    }

    const auto find = [&](const graphics::shader_bindings& bindings)
    {
        const std::array stages{ stage_bindings{ .stage = shader_stage::compute, .bindings = &bindings } };
        return registry.find_compatible(stages);
    };

    SECTION("both fit: the first in registration order")
    {
        const auto signature = find(constant_buffer(0, 16));
        REQUIRE(signature.has_value());
        CHECK(*signature == *standard);
    }

    SECTION("only the second fits")
    {
        graphics::shader_bindings bindings = constant_buffer(0, 16);
        bindings.buffers.push_back({ .kind = graphics::buffer_binding_kind::unordered_access, .shader_register = 0 });
        const auto signature = find(bindings);
        REQUIRE(signature.has_value());
        CHECK(*signature == *uav_model);
    }

    SECTION("neither fits: every candidate is named with its mismatch")
    {
        const auto signature = find(sampler(7));
        REQUIRE_FALSE(signature.has_value());
        CHECK(graphics::is_error(signature.error(), graphics::error_kind::no_compatible_root_signature));
        CHECK(contains(signature.error().message, "standard: compute shader s7 space0: no static sampler"));
        CHECK(contains(signature.error().message, "uav model: compute shader s7 space0: no static sampler"));
    }

    SECTION("check_compatible on the standard signature")
    {
        const graphics::shader_bindings pass = constant_buffer(1, 256);
        const std::array fits{ stage_bindings{ .stage = shader_stage::pixel, .bindings = &pass } };
        CHECK(registry.check_compatible(*standard, fits).has_value());

        const graphics::shader_bindings uav = buffer(graphics::buffer_binding_kind::unordered_access, 0);
        const std::array misfits{ stage_bindings{ .stage = shader_stage::compute, .bindings = &uav } };
        const auto checked = registry.check_compatible(*standard, misfits);
        REQUIRE_FALSE(checked.has_value());
        CHECK(graphics::is_error(checked.error(), graphics::error_kind::root_signature_incompatible));
        CHECK(
            checked.error().message == "root signature 'standard': compute shader u0 space0: no root unordered access");
    }

    SECTION("check_compatible on a signature that isn't registered")
    {
        const auto unregistered = rhi::pipelines{ *hardware_interface }.create_root_signature(
            graphics::binding_layout::standard_description());
        REQUIRE(unregistered.has_value());
        const auto checked = registry.check_compatible(*unregistered, {});
        REQUIRE_FALSE(checked.has_value());
        CHECK(graphics::is_error(checked.error(), graphics::error_kind::invalid_argument));
    }

    SECTION("lookups are counted")
    {
        const std::uint64_t before = registry.lookup_count();
        CHECK(find(constant_buffer(0, 16)).has_value());
        CHECK(registry.check_compatible(*standard, {}).has_value());
        CHECK(registry.lookup_count() == before + 2);
    }
}

TEST_CASE("root_signature_registry::add checks the candidate's name", "[graphics]")
{
    tests::test_rhi hardware_interface;
    graphics::root_signature_registry registry{ rhi::pipelines{ *hardware_interface } };

    SECTION("an empty registry finds nothing")
    {
        const auto signature = registry.find_compatible({});
        REQUIRE_FALSE(signature.has_value());
        CHECK(graphics::is_error(signature.error(), graphics::error_kind::no_compatible_root_signature));
    }

    SECTION("empty and duplicate names")
    {
        const auto empty = registry.add("", graphics::binding_layout::standard_description());
        REQUIRE_FALSE(empty.has_value());
        CHECK(graphics::is_error(empty.error(), graphics::error_kind::invalid_argument));

        REQUIRE(registry.add("standard", graphics::binding_layout::standard_description()).has_value());
        const auto duplicate = registry.add("standard", graphics::binding_layout::standard_description());
        REQUIRE_FALSE(duplicate.has_value());
        CHECK(graphics::is_error(duplicate.error(), graphics::error_kind::invalid_argument));
        CHECK(contains(duplicate.error().message, "'standard' is already registered"));
    }

    SECTION("the RHI's errors pass through")
    {
        const std::array parameters{ constants(0, 65) }; // more than D3D12's 64-DWORD root signature limit
        const auto signature = registry.add("too big", { .parameters = parameters });
        REQUIRE_FALSE(signature.has_value());
        CHECK(signature.error().domain == error_domain::rhi);
    }
}
