// The Avis standard binding layout (docs/tasks/08-graphics-shaders-and-pipelines.md §8.4)

#include "graphics_test_binding_layout_shader.h"
#include "helpers/graphics_fixture.h"
#include "helpers/rhi_fixture.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/frame_token.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/root_signature.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/types.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

using namespace avis;
namespace binding_layout = avis::graphics::binding_layout;

namespace
{
    struct sixteen_dwords
    {
        std::array<std::uint32_t, 16> values;
    };

    struct seventeen_dwords
    {
        std::array<std::uint32_t, 17> values;
    };

    struct six_bytes
    {
        std::uint16_t values[3];
    };

    template<class Constants>
    concept pushable = requires(rhi::compute_recorder& recorder, const Constants& constants) {
        binding_layout::push_draw_constants(recorder, constants);
    };

    template<class Recorder>
    concept binds_pass_constants =
        requires(Recorder& recorder) { binding_layout::set_pass_constants(recorder, rhi::gpu_address{}); };

    static_assert(pushable<sixteen_dwords>);
    static_assert(!pushable<seventeen_dwords>);
    static_assert(!pushable<six_bytes>);
    static_assert(binds_pass_constants<rhi::graphics_recorder>);
    static_assert(binds_pass_constants<rhi::compute_recorder>);
    static_assert(!binds_pass_constants<rhi::copy_recorder>);
} // namespace

TEST_CASE("the standard description has the Avis binding model", "[graphics][unit]")
{
    const rhi::root_signature_description description = binding_layout::standard_description();

    REQUIRE(description.parameters.size() == 3);
    const rhi::root_parameter& draw = description.parameters[binding_layout::draw_constants_parameter];
    CHECK(draw.kind == rhi::root_parameter_kind::constants);
    CHECK(draw.shader_register == 0);
    CHECK(draw.register_space == 0);
    CHECK(draw.constant_count == 16);
    CHECK(draw.visibility == rhi::shader_visibility::all);

    const rhi::root_parameter& pass = description.parameters[binding_layout::pass_constants_parameter];
    CHECK(pass.kind == rhi::root_parameter_kind::constant_buffer);
    CHECK(pass.shader_register == 1);
    CHECK(pass.register_space == 0);
    CHECK(pass.visibility == rhi::shader_visibility::all);

    const rhi::root_parameter& view = description.parameters[binding_layout::view_constants_parameter];
    CHECK(view.kind == rhi::root_parameter_kind::constant_buffer);
    CHECK(view.shader_register == 2);
    CHECK(view.register_space == 0);
    CHECK(view.visibility == rhi::shader_visibility::all);

    REQUIRE(description.static_samplers.size() == 6);
    for (std::uint32_t index = 0; index < 6; ++index)
    {
        CAPTURE(index);
        CHECK(description.static_samplers[index].shader_register == index);
        CHECK(description.static_samplers[index].register_space == 0);
        CHECK(description.static_samplers[index].visibility == rhi::shader_visibility::all);
    }

    const auto sampler = [&](const binding_layout::static_sampler slot)
    { return description.static_samplers[static_cast<std::uint32_t>(slot)].sampler; };

    CHECK(sampler(binding_layout::static_sampler::point_clamp).minification == rhi::filter::point);
    CHECK(sampler(binding_layout::static_sampler::point_clamp).address_u == rhi::address_mode::clamp);
    CHECK(sampler(binding_layout::static_sampler::linear_clamp).minification == rhi::filter::linear);
    CHECK(sampler(binding_layout::static_sampler::linear_clamp).address_v == rhi::address_mode::clamp);
    CHECK(sampler(binding_layout::static_sampler::linear_wrap).minification == rhi::filter::linear);
    CHECK(sampler(binding_layout::static_sampler::linear_wrap).address_w == rhi::address_mode::wrap);
    CHECK(sampler(binding_layout::static_sampler::anisotropic_wrap).minification == rhi::filter::anisotropic);
    CHECK(sampler(binding_layout::static_sampler::anisotropic_wrap).maximum_anisotropy == 16);
    CHECK(sampler(binding_layout::static_sampler::point_wrap).magnification == rhi::filter::point);
    CHECK(sampler(binding_layout::static_sampler::point_wrap).address_u == rhi::address_mode::wrap);

    const rhi::sampler_description shadow = sampler(binding_layout::static_sampler::shadow_comparison);
    CHECK(shadow.minification == rhi::filter::linear);
    CHECK(shadow.magnification == rhi::filter::linear);
    REQUIRE(shadow.compare.has_value());
    CHECK(*shadow.compare == rhi::comparison::greater_equal);

    // Only the shadow sampler compares
    for (std::uint32_t index = 0; index < 6; ++index)
    {
        CAPTURE(index);
        CHECK(
            description.static_samplers[index].sampler.compare.has_value() ==
            (index == static_cast<std::uint32_t>(binding_layout::static_sampler::shadow_comparison)));
    }

    // Static storage: two calls see the same arrays
    CHECK(binding_layout::standard_description().parameters.data() == description.parameters.data());
    CHECK(binding_layout::standard_description().static_samplers.data() == description.static_samplers.data());
}

TEST_CASE("a compute pipeline on the standard signature reads all three parameters", "[graphics]")
{
    struct draw_constants
    {
        std::uint32_t buffer_index;
        std::uint32_t count;
        std::uint32_t base;
        std::uint32_t multiplier;
    };
    struct pass_constants
    {
        std::uint32_t offset;
        std::uint32_t unused;
    };
    struct view_constants
    {
        std::uint32_t offset;
    };

    tests::test_rhi hardware_interface;
    rhi::pipelines pipelines{ *hardware_interface };
    rhi::resources resources{ *hardware_interface };

    const auto signature = pipelines.create_root_signature(binding_layout::standard_description());
    REQUIRE(signature.has_value());
    const auto pipeline = pipelines.create_pipeline(
        rhi::compute_pipeline_description{ .root_signature = *signature,
                                           .compute_shader =
                                               std::as_bytes(std::span{ graphics_test_binding_layout_shader }),
                                           .debug_name = "binding layout test" });
    REQUIRE(pipeline.has_value());

    constexpr std::uint32_t count = 100;
    const auto target = resources.create_buffer(
        { .size = count * 4u,
          .usage = rhi::buffer_usage::unordered_access | rhi::buffer_usage::copy_source,
          .debug_name = "binding layout target" });
    REQUIRE(target.has_value());
    const auto readback = resources.create_buffer(
        { .size = count * 4u,
          .usage = rhi::buffer_usage::copy_destination,
          .memory = rhi::memory_type::readback,
          .debug_name = "binding layout readback" });
    REQUIRE(readback.has_value());

    auto scheduler = tests::make_test_frame_scheduler(hardware_interface);
    graphics::frame_token frame = tests::begin_frame_inline(*scheduler);
    const auto pass = scheduler->write_frame_constants(frame, pass_constants{ .offset = 1000, .unused = 0 });
    REQUIRE(pass.has_value());
    const auto view = scheduler->write_frame_constants(frame, view_constants{ .offset = 20000 });
    REQUIRE(view.has_value());

    auto list = rhi::command_lists{ *hardware_interface }.acquire<rhi::queue_type::compute>("binding layout");
    REQUIRE(list.has_value());
    list->set_pipeline(*pipeline);
    binding_layout::push_draw_constants(
        *list,
        draw_constants{ .buffer_index = resources.uav(*target).value, .count = count, .base = 5, .multiplier = 3 });
    binding_layout::set_pass_constants(*list, *pass);
    binding_layout::set_view_constants(*list, *view);
    list->dispatch((count + 63) / 64, 1, 1);
    const std::array barriers{ rhi::buffer_barrier{
        .buffer = *target,
        .before = { rhi::pipeline_synchronization::compute_shading, rhi::resource_access::unordered_access },
        .after = { rhi::pipeline_synchronization::copy, rhi::resource_access::copy_source } } };
    list->barrier({}, barriers, {});
    list->copy_buffer(*readback, 0, *target, 0, count * 4u);

    const auto point = rhi::queue<rhi::queue_type::compute>{ *hardware_interface }.submit(std::span{ &*list, 1 }, {});
    REQUIRE(point.has_value());
    hardware_interface.wait_for(*point);
    scheduler->end_frame(std::move(frame), std::span{ &*point, 1 });

    const std::span<std::byte> bytes = resources.mapped_bytes(*readback);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + index * 4, 4);
        CAPTURE(index);
        REQUIRE(value == 5 + index * 3 + 1000 + 20000);
    }
}
