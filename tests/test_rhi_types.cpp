// The RHI's public value types (docs/tasks/06-rhi.md §6.2): handles, formats, errors, barriers, synchronization
// points and the description defaults. No device.

#include "avis/rhi/barriers.h"
#include "avis/rhi/configuration.h"
#include "avis/rhi/error.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/memory.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/root_signature.h"
#include "avis/rhi/types.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>

// Test-only definition until the slot map defines it privately (6.6)
namespace avis::rhi::detail
{
    struct handle_access
    {
        template<class Tag>
        static constexpr handle<Tag> make(const std::uint32_t index, const std::uint32_t generation) noexcept
        {
            handle<Tag> value;
            value.index_ = index;
            value.generation_ = generation;
            return value;
        }
    };
} // namespace avis::rhi::detail

namespace
{
    using avis::rhi::format;
    using avis::rhi::format_traits;

    template<class Handle>
    struct handle_tag;

    template<class Tag>
    struct handle_tag<avis::rhi::handle<Tag>>
    {
        using type = Tag;
    };

    template<class Handle>
    constexpr Handle handle_from(const std::uint32_t index, const std::uint32_t generation) noexcept
    {
        return avis::rhi::detail::handle_access::make<typename handle_tag<Handle>::type>(index, generation);
    }

    constexpr format_traits texel(const std::uint8_t bytes) noexcept
    {
        return { .bytes_per_block = bytes };
    }

    constexpr format_traits block(const std::uint8_t bytes, const bool srgb = false) noexcept
    {
        return { .bytes_per_block = bytes, .block_width = 4, .block_height = 4, .srgb = srgb, .compressed = true };
    }

    struct expected_format
    {
        format value;
        std::string_view name;
        format_traits traits;
    };

    // The table of 09 §3.5, one row per enumerator in declaration order
    constexpr std::array expected_formats{
        expected_format{ format::unknown, "unknown", {} },
        expected_format{ format::rgba8_unorm, "rgba8_unorm", texel(4) },
        expected_format{ format::rgba8_unorm_srgb, "rgba8_unorm_srgb", { .bytes_per_block = 4, .srgb = true } },
        expected_format{ format::bgra8_unorm, "bgra8_unorm", texel(4) },
        expected_format{ format::rgb10a2_unorm, "rgb10a2_unorm", texel(4) },
        expected_format{ format::r11g11b10_float, "r11g11b10_float", texel(4) },
        expected_format{ format::rgba16_float, "rgba16_float", texel(8) },
        expected_format{ format::rg16_float, "rg16_float", texel(4) },
        expected_format{ format::rg16_unorm, "rg16_unorm", texel(4) },
        expected_format{ format::rg16_snorm, "rg16_snorm", texel(4) },
        expected_format{ format::r16_float, "r16_float", texel(2) },
        expected_format{ format::r16_uint, "r16_uint", texel(2) },
        expected_format{ format::r8_unorm, "r8_unorm", texel(1) },
        expected_format{ format::r8_uint, "r8_uint", texel(1) },
        expected_format{ format::r32_float, "r32_float", texel(4) },
        expected_format{ format::r32_uint, "r32_uint", texel(4) },
        expected_format{ format::rg32_float, "rg32_float", texel(8) },
        expected_format{ format::rgba32_float, "rgba32_float", texel(16) },
        expected_format{ format::rgba32_uint, "rgba32_uint", texel(16) },
        expected_format{ format::d32_float, "d32_float", { .bytes_per_block = 4, .depth = true } },
        expected_format{ format::bc1_unorm, "bc1_unorm", block(8) },
        expected_format{ format::bc1_unorm_srgb, "bc1_unorm_srgb", block(8, true) },
        expected_format{ format::bc3_unorm, "bc3_unorm", block(16) },
        expected_format{ format::bc3_unorm_srgb, "bc3_unorm_srgb", block(16, true) },
        expected_format{ format::bc4_unorm, "bc4_unorm", block(8) },
        expected_format{ format::bc5_unorm, "bc5_unorm", block(16) },
        expected_format{ format::bc7_unorm, "bc7_unorm", block(16) },
        expected_format{ format::bc7_unorm_srgb, "bc7_unorm_srgb", block(16, true) },
    };

    static_assert(expected_formats.back().value == format::bc7_unorm_srgb, "the table must end at the last enumerator");
} // namespace

TEST_CASE("handles: default is invalid, tags are distinct types, std::hash works", "[rhi][unit]")
{
    using namespace avis::rhi;

    static_assert(!std::is_same_v<graphics_pipeline_handle, compute_pipeline_handle>);
    static_assert(!std::is_same_v<texture_handle, buffer_handle>);
    static_assert(std::is_trivially_copyable_v<texture_handle>);
    static_assert(!texture_handle{}.is_valid());
    static_assert(handle_from<texture_handle>(3, 1).is_valid());
    static_assert(handle_from<texture_handle>(3, 1) == handle_from<texture_handle>(3, 1));
    static_assert(handle_from<texture_handle>(3, 1) != handle_from<texture_handle>(3, 2));
    static_assert(handle_from<texture_handle>(3, 1) != handle_from<texture_handle>(4, 1));

    CHECK_FALSE(buffer_handle{}.is_valid());
    CHECK_FALSE(root_signature_handle{}.is_valid());

    std::unordered_set<texture_handle> handles;
    handles.insert(handle_from<texture_handle>(1, 1));
    handles.insert(handle_from<texture_handle>(1, 3)); // same slot, later generation
    handles.insert(handle_from<texture_handle>(2, 1));
    handles.insert(handle_from<texture_handle>(1, 1)); // duplicate

    CHECK(handles.size() == 3);
    CHECK(handles.contains(handle_from<texture_handle>(1, 3)));
    CHECK_FALSE(handles.contains(handle_from<texture_handle>(1, 2)));
    CHECK_FALSE(handles.contains(texture_handle{}));
}

TEST_CASE("format traits match the table for every enumerator, and to_string names each", "[rhi][unit]")
{
    std::set<std::string_view> names;
    for (std::size_t index = 0; index < expected_formats.size(); ++index)
    {
        const auto& expected = expected_formats[index];
        INFO("format " << expected.name);
        CHECK(static_cast<std::size_t>(expected.value) == index);
        CHECK(avis::rhi::traits(expected.value) == expected.traits);
        CHECK(avis::rhi::to_string(expected.value) == expected.name);
        names.insert(avis::rhi::to_string(expected.value));
    }
    CHECK(names.size() == expected_formats.size());

    // One past the last enumerator
    CHECK(avis::rhi::to_string(static_cast<format>(expected_formats.size())) == "invalid");
    CHECK(avis::rhi::traits(static_cast<format>(expected_formats.size())) == format_traits{});

    static_assert(avis::rhi::traits(format::bc7_unorm).block_width == 4);
    static_assert(avis::rhi::traits(format::d32_float).depth);
    static_assert(avis::rhi::traits(format::rgba8_unorm_srgb).srgb);
}

TEST_CASE("RHI errors are in error_domain::rhi and print their kind", "[rhi][unit]")
{
    using avis::rhi::error_kind;

    const avis::error error = avis::rhi::make_error(error_kind::invalid_handle, "stale texture handle");

    CHECK(error.domain == avis::error_domain::rhi);
    CHECK(error.code == static_cast<std::int32_t>(error_kind::invalid_handle));
    CHECK(error.message == "stale texture handle");
    CHECK(avis::rhi::is_error(error, error_kind::invalid_handle));
    CHECK_FALSE(avis::rhi::is_error(error, error_kind::invalid_argument));

    // Same code in another domain isn't an RHI error
    const avis::error generic{ .domain = avis::error_domain::generic, .code = error.code };
    CHECK_FALSE(avis::rhi::is_error(generic, error_kind::invalid_handle));

    CHECK(std::format("{}", error_kind::invalid_handle) == "invalid_handle");
    CHECK(
        std::format("{}", error)
            .starts_with(
                std::format("[rhi:{}] stale texture handle", static_cast<std::int32_t>(error_kind::invalid_handle))));

    for (std::int32_t value = 0; value <= static_cast<std::int32_t>(error_kind::native_error); ++value)
    {
        CHECK(avis::rhi::to_string(static_cast<error_kind>(value)) != "unknown");
    }

    static_assert(std::is_base_of_v<avis::runtime_error, avis::rhi::device_lost>);
    const avis::rhi::device_lost lost{ avis::rhi::make_error(error_kind::device_lost, "removed") };
    CHECK(avis::rhi::is_error(lost.error(), error_kind::device_lost));
}

TEST_CASE("access_states are constant expressions with the documented values", "[rhi][unit]")
{
    using namespace avis::rhi;

    constexpr access_state shader_resource = access_states::shader_resource;
    static_assert(shader_resource.layout == texture_layout::shader_resource);
    static_assert(
        shader_resource.synchronization ==
        (pipeline_synchronization::pixel_shading | pipeline_synchronization::compute_shading));
    static_assert(shader_resource.access == avis::flags<resource_access>(resource_access::shader_resource));

    static_assert(access_states::undefined.layout == texture_layout::undefined);
    static_assert(access_states::undefined.access == avis::flags<resource_access>(resource_access::no_access));
    static_assert(access_states::common.access.empty()); // common is 0
    static_assert(access_states::copy_destination.layout == texture_layout::copy_destination);

    // The default state is {none, no_access, common}
    const access_state default_state;
    CHECK(default_state.synchronization.empty());
    CHECK(default_state.access == avis::flags<resource_access>(resource_access::no_access));
    CHECK(default_state.layout == texture_layout::common);

    // D3D12_BARRIER_SYNC / D3D12_BARRIER_ACCESS values, checked against D3D12 itself in the conversions (6.3)
    static_assert(static_cast<std::uint32_t>(pipeline_synchronization::clear_unordered_access_view) == 0x8000);
    static_assert(static_cast<std::uint32_t>(resource_access::no_access) == 0x8000'0000);
}

TEST_CASE("latest picks the later point", "[rhi][unit]")
{
    using avis::rhi::latest;
    using avis::rhi::queue_type;
    using avis::rhi::synchronization_point;

    constexpr synchronization_point earlier{ queue_type::compute, 4 };
    constexpr synchronization_point later{ queue_type::compute, 9 };

    static_assert(latest(earlier, later) == later);
    static_assert(latest(later, earlier) == later);
    static_assert(latest(later, later) == later);

    // A point with value 0 waits for nothing, whatever its queue
    static_assert(latest(synchronization_point{}, earlier) == earlier);
    static_assert(latest(earlier, synchronization_point{}) == earlier);
    static_assert(latest(synchronization_point{}, synchronization_point{}).value == 0);

    CHECK(
        latest(synchronization_point{ queue_type::copy, 2 }, synchronization_point{ queue_type::copy, 1 }).value == 2);
}

TEST_CASE("description and configuration defaults", "[rhi][unit]")
{
    using namespace avis::rhi;

    const buffer_description buffer;
    CHECK(buffer.usage.empty());
    CHECK(buffer.memory == memory_type::gpu_only);

    const texture_description texture;
    CHECK(texture.usage == avis::flags<texture_usage>(texture_usage::shader_resource));
    CHECK(texture.initial_layout == texture_layout::common);
    CHECK_FALSE(texture.optimized_clear.has_value());

    const auto usage = texture_usage::render_target | texture_usage::shader_resource;
    CHECK(usage.count(texture_usage::render_target) == 1);
    CHECK(usage.count(texture_usage::unordered_access) == 0);

    static_assert(all_subresources.mip_count == subresource_range::all);
    static_assert(all_subresources.layer_count == subresource_range::all);

    const render_hardware_interface_configuration configuration;
    CHECK_FALSE(configuration.adapter.adapter.has_value());
    CHECK(configuration.descriptor_heaps.persistent_descriptor_count == 900'000);
    CHECK(configuration.descriptor_heaps.transient_descriptor_count == 100'000);
    CHECK(configuration.descriptor_heaps.sampler_count == 2'048);

    const graphics_pipeline_state state;
    CHECK(state.depth.compare == comparison::greater_equal);
    CHECK(state.render_target_formats[7] == format::unknown);

    const heap_description heap;
    CHECK(heap.kind == heap_kind::all_resources);

    const root_parameter parameter;
    CHECK(parameter.visibility == shader_visibility::all);

    static_assert(gpu_address{ 256 }.offset_by(64) == gpu_address{ 320 });
    static_assert(bindless_index{} == bindless_index{ 0 });
}
