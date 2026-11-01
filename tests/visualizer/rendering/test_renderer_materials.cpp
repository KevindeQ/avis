// Textures and materials (docs/tasks/11-renderer.md §11.4): the GPU mip chain of decoded images, the default textures
// a material draws with until its textures are resident, the material table's slots and their retirement. On the
// renderer fixture, with the debug layer and GPU-based validation.

#include "helpers/log_capture.h"
#include "helpers/renderer_fixture.h"
#include "rendering/material_table.h"
#include "rendering/scene_renderer.h"
#include "rendering/texture_residency.h"

#include "avis/graphics/binding_layout.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/readback_data.h"
#include "avis/rhi/formats.h"
#include "avis/scene/assets/material.h"
#include "avis/scene/assets/texture_data.h"
#include "avis/scene/snapshot.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <shared_types.hlsli>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::renderer_fixture;

namespace
{
    // A width × height one-texel checkerboard of black and white, with mip 0 only and the chain to generate
    scene::texture_data make_checkerboard(const std::uint32_t size, const scene::texture_format format)
    {
        scene::texture_data data{ .width = size, .height = size, .format = format, .generate_mips = true };
        std::vector<std::byte> texels(static_cast<std::size_t>(size) * size * 4);
        for (std::uint32_t y = 0; y < size; ++y)
        {
            for (std::uint32_t x = 0; x < size; ++x)
            {
                const auto value = static_cast<std::byte>(((x + y) % 2 == 0) ? 255 : 0);
                std::byte* texel = texels.data() + (static_cast<std::size_t>(y) * size + x) * 4;
                texel[0] = texel[1] = texel[2] = value;
                texel[3] = std::byte{ 255 };
            }
        }
        data.mips.push_back(std::move(texels));
        return data;
    }

    // The handles of the latest snapshot's first material and texture
    struct listed_assets
    {
        scene::material_handle material;
        scene::texture_handle texture;
    };

    listed_assets first_assets(renderer_fixture& fixture)
    {
        const scene::snapshot_reference snapshot = fixture.snapshots.acquire();
        REQUIRE(static_cast<bool>(snapshot));
        listed_assets found;
        if (!snapshot->materials.empty())
        {
            found.material = snapshot->materials[0].handle;
        }
        if (!snapshot->textures.empty())
        {
            found.texture = snapshot->textures[0].handle;
        }
        return found;
    }

    // A cube whose material's base colour is `texture`; the fixture's material keeps the texture alive, so the
    // test's own reference is released
    void add_textured_cube(renderer_fixture& fixture, scene::texture_data texture, scene::material_description material)
    {
        const result<scene::texture_handle> handle = fixture.assets.create_texture(std::move(texture));
        REQUIRE(handle.has_value());
        material.base_color_texture = *handle;
        fixture.add_unit_cube({ 0.0f, 0.0f, -5.0f }, material);
        fixture.assets.release(*handle);
    }

    // Every texel of an RGBA8 mip, RGB within `tolerance` of `expected`
    void check_uniform(const graphics::readback_image& image, const int expected, const int tolerance)
    {
        for (std::uint32_t y = 0; y < image.height(); ++y)
        {
            const std::byte* row = image.bytes().data() + static_cast<std::size_t>(y) * image.row_pitch();
            for (std::uint32_t x = 0; x < image.width(); ++x)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    const int value = std::to_integer<int>(row[x * 4 + static_cast<std::uint32_t>(channel)]);
                    INFO("texel " << x << ", " << y << " channel " << channel);
                    REQUIRE(std::abs(value - expected) <= tolerance);
                }
                REQUIRE(std::to_integer<int>(row[x * 4 + 3]) == 255);
            }
        }
    }
} // namespace

TEST_CASE("full mip chains and scene texture formats", "[renderer][unit]")
{
    CHECK(full_mip_count(1, 1) == 1);
    CHECK(full_mip_count(2, 1) == 2);
    CHECK(full_mip_count(256, 256) == 9);
    CHECK(full_mip_count(300, 20) == 9); // 300 → 150 → 75 → 37 → 18 → 9 → 4 → 2 → 1
    CHECK(to_rhi_format(scene::texture_format::rgba8_srgb) == rhi::format::rgba8_unorm_srgb);
    CHECK(to_rhi_format(scene::texture_format::rgba8_unorm) == rhi::format::rgba8_unorm);
    CHECK(to_rhi_format(scene::texture_format::bc7_srgb) == rhi::format::bc7_unorm_srgb);
}

TEST_CASE_METHOD(renderer_fixture, "a decoded texture's mip chain is generated on the GPU", "[renderer]")
{
    const bool srgb = GENERATE(false, true);
    INFO((srgb ? "sRGB" : "linear"));
    add_textured_cube(
        *this,
        make_checkerboard(256, srgb ? scene::texture_format::rgba8_srgb : scene::texture_format::rgba8_unorm),
        {});
    publish();
    const listed_assets listed = first_assets(*this);

    const std::vector views{ view() };
    render_and_read(views); // uploads mip 0 and generates the chain; materials see it from the next frame on
    CHECK(renderer->debug_information().textures.mip_chains_generated == 1);
    render_and_read(views);
    const resident_texture* resident = renderer->textures().find(listed.texture);
    REQUIRE(resident != nullptr);
    CHECK(resident->mip_levels == 9);
    CHECK(resident->format == (srgb ? rhi::format::rgba8_unorm_srgb : rhi::format::rgba8_unorm));
    CHECK(renderer->debug_information().textures.mip_chains_generated == 1); // once

    // Mip 0 is the checkerboard as uploaded
    const graphics::readback_image mip0 = read_subresource(resident->texture);
    REQUIRE(mip0.width() == 256);
    CHECK(std::to_integer<int>(mip0.bytes()[0]) == 255);
    CHECK(std::to_integer<int>(mip0.bytes()[4]) == 0);

    // Mip 4 (16 × 16) averages black and white: 0.5 in linear, which is 188 when the texels are sRGB-encoded, not 128
    const graphics::readback_image mip4 = read_subresource(resident->texture, { .mip = 4 });
    REQUIRE(mip4.width() == 16);
    REQUIRE(mip4.height() == 16);
    check_uniform(mip4, srgb ? 188 : 128, 1);

    // And so on down to 1 × 1
    const graphics::readback_image last = read_subresource(resident->texture, { .mip = 8 });
    REQUIRE(last.width() == 1);
    check_uniform(last, srgb ? 188 : 128, 1);
}

TEST_CASE_METHOD(renderer_fixture, "a material draws with default textures until its texture is resident", "[renderer]")
{
    scene::material_description description;
    description.base_color_factor = { 0.5f, 0.25f, 1.0f, 0.75f };
    description.metallic_factor = 0.3f;
    description.roughness_factor = 0.6f;
    description.emissive_factor = Eigen::Vector3f(1.0f, 2.0f, 3.0f);
    description.shading = scene::shading_model::unlit;
    description.alpha = scene::alpha_mode::blend;
    description.blend = scene::blend_operation::additive;
    description.double_sided = true;
    description.point_size = 0.2f;
    description.line_width = 3.0f;
    add_textured_cube(*this, make_checkerboard(64, scene::texture_format::rgba8_srgb), description);
    publish();
    const listed_assets listed = first_assets(*this);
    const std::vector views{ view() };

    // Frame 1: the texture uploads and gets its chain; the slot points to the white default meanwhile
    render_and_read(views);
    const resident_material* first = renderer->materials().find(listed.material);
    REQUIRE(first != nullptr);
    const std::uint32_t first_slot = first->slot;
    CHECK(first_slot != material_table::default_slot);
    CHECK(renderer->materials().slot(listed.material) == first_slot);
    CHECK(first->data.base_color_texture == renderer->textures().default_srv(default_texture::white).value);
    CHECK(first->data.normal_texture == 0); // no texture
    CHECK(first->textures[0].version == 0); // a default isn't a reference

    // The other fields, as the description says
    CHECK(first->data.base_color_factor.y == 0.25f);
    CHECK(first->data.base_color_factor.w == 0.75f);
    CHECK(first->data.metallic_factor == 0.3f);
    CHECK(first->data.roughness_factor == 0.6f);
    CHECK(first->data.emissive_factor.z == 3.0f);
    CHECK(first->data.shading_model == gpu::shading_model_unlit);
    CHECK(first->data.alpha_mode == gpu::alpha_mode_blend);
    CHECK(first->data.blend_operation == gpu::blend_operation_additive);
    CHECK(first->data.flags == gpu::material_flag_double_sided);
    CHECK(first->data.point_size == 0.2f);
    CHECK(first->data.line_width == 3.0f);
    CHECK(
        first->data.sampler_index ==
        static_cast<std::uint32_t>(graphics::binding_layout::static_sampler::anisotropic_wrap));

    // Frame 2: the texture is current, so the material is written once more, into a new slot
    render_and_read(views);
    const resident_texture* texture = renderer->textures().find(listed.texture);
    REQUIRE(texture != nullptr);
    const resident_material* second = renderer->materials().find(listed.material);
    REQUIRE(second != nullptr);
    CHECK(second->data.base_color_texture == texture->srv.value);
    CHECK(second->textures[0].handle == listed.texture);
    CHECK(second->textures[0].version == texture->version);
    const std::uint32_t second_slot = second->slot;
    CHECK(second_slot != first_slot);
    CHECK(renderer->debug_information().materials.rewrites == 1);

    // Nothing changes after that, and the first slot goes once its frames retire
    render_and_read(views, 4);
    CHECK(renderer->debug_information().materials.rewrites == 1);
    CHECK(renderer->materials().versions(listed.material) == std::vector<std::uint64_t>{ 1 });
    CHECK(renderer->materials().slot(listed.material) == second_slot);
    CHECK(renderer->debug_information().materials.slots_used == 2); // the default and this one
}

TEST_CASE_METHOD(renderer_fixture, "the default textures and the default material", "[renderer]")
{
    const std::vector views{ view() };
    render_and_read(views); // flushes the defaults' uploads

    const auto texel_of = [&](const default_texture which)
    {
        const graphics::readback_image image = read_subresource(renderer->textures().default_resident(which).texture);
        REQUIRE(image.width() == 1);
        std::array<int, 4> texel{};
        for (std::size_t channel = 0; channel < 4; ++channel)
        {
            texel[channel] = std::to_integer<int>(image.bytes()[channel]);
        }
        return texel;
    };
    CHECK(texel_of(default_texture::white) == std::array{ 255, 255, 255, 255 });
    CHECK(texel_of(default_texture::black) == std::array{ 0, 0, 0, 255 });
    CHECK(texel_of(default_texture::flat_normal) == std::array{ 128, 128, 255, 255 });
    CHECK(texel_of(default_texture::missing) == std::array{ 255, 0, 255, 255 });

    CHECK(renderer->materials().slot({}) == material_table::default_slot);
    CHECK(renderer->debug_information().materials.slots_used == 1);
    CHECK(renderer->materials().frame_ready_after().value != 0);
}

TEST_CASE_METHOD(renderer_fixture, "a texture that can't be made resident draws as missing", "[renderer]")
{
    tests::log_capture capture{ "renderer" };
    add_textured_cube(*this, scene::texture_data{ .width = 0, .height = 0 }, {});
    publish();
    const listed_assets listed = first_assets(*this);

    const std::vector views{ view() };
    render_and_read(views, 2);
    CHECK(renderer->textures().status(listed.texture) == texture_status::failed);
    const resident_material* material = renderer->materials().find(listed.material);
    REQUIRE(material != nullptr);
    CHECK(material->data.base_color_texture == renderer->textures().default_srv(default_texture::missing).value);
    CHECK(capture.contains("it has no texels"));
    CHECK(renderer->debug_information().materials.rewrites == 0); // not retried
}

TEST_CASE_METHOD(renderer_fixture, "1,000 material versions over 100 frames keep the slot count bounded", "[renderer]")
{
    constexpr int material_count = 10;
    for (int index = 0; index < material_count; ++index)
    {
        add_unit_cube({ static_cast<float>(index), 0.0f, -5.0f });
    }
    publish();
    std::vector<scene::material_handle> materials;
    {
        const scene::snapshot_reference snapshot = snapshots.acquire();
        for (const scene::versioned_material_entry& listed : snapshot->materials)
        {
            materials.push_back(listed.handle);
        }
    }
    REQUIRE(materials.size() == material_count);

    const std::vector views{ view() };
    const std::uint32_t frames_in_flight = graphics.frames->frames_in_flight();
    // The live materials, plus the superseded slots of the frames that may still read them, plus the default
    const std::uint32_t bound = 1 + material_count * (frames_in_flight + 2);
    std::uint32_t most = 0;
    for (int frame = 0; frame < 100; ++frame)
    {
        for (const scene::material_handle material : materials)
        {
            scene::material_description description;
            description.base_color_factor = { static_cast<float>(frame) / 100.0f, 0.5f, 0.5f, 1.0f };
            REQUIRE(assets.update_material(material, description).has_value());
        }
        publish();
        render_and_read(views);
        most = std::max(most, renderer->debug_information().materials.slots_used);
    }

    const material_table_statistics statistics = renderer->debug_information().materials;
    INFO("most slots used: " << most << ", bound " << bound);
    CHECK(most <= bound);
    // The default, then 1,000 versions: the first frame already renders version 2, so version 1 is never written
    CHECK(statistics.writes == 1 + 1000);
    CHECK(statistics.materials == material_count);
    for (const scene::material_handle material : materials)
    {
        const resident_material* current = renderer->materials().find(material);
        REQUIRE(current != nullptr);
        CHECK(current->version == 101);
        CHECK(current->data.base_color_factor.x == 0.99f);
    }
}
