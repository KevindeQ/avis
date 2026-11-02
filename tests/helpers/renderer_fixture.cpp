#include "renderer_fixture.h"

#include "graphics_fixture.h"
#include "rendering/renderer_shaders.h"

#include "avis/graphics/readback_data.h"
#include "avis/rhi/formats.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/rendering.h"
#include "avis/threading/parallel_helpers.h"

#include <Eigen/Geometry>
#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <numbers>
#include <utility>

#ifndef AVIS_TEST_RENDERER_SHADER_DIRECTORY
#    error "tests/CMakeLists.txt defines AVIS_TEST_RENDERER_SHADER_DIRECTORY, where the renderer's shaders are built"
#endif

namespace avis::tests
{
    namespace
    {
        namespace rendering = visualizer::rendering;

        scene::mesh_data build(scene::mesh_builder&& builder)
        {
            result<scene::mesh_data> built = std::move(builder).build();
            REQUIRE(built.has_value());
            return std::move(*built);
        }

        // Two triangles over the corners `origin`, origin + u, origin + u + v, origin + v, all with `normal`
        void add_quad(
            scene::mesh_builder& builder,
            std::uint32_t& first_vertex,
            const Eigen::Vector3f& origin,
            const Eigen::Vector3f& u,
            const Eigen::Vector3f& v,
            const Eigen::Vector3f& normal)
        {
            const Eigen::Vector3f corners[] = { origin, origin + u, origin + u + v, origin + v };
            const Eigen::Vector2f uvs[] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };
            for (int corner = 0; corner < 4; ++corner)
            {
                builder.add_vertex(corners[corner]).normal(normal).uv(uvs[corner]);
            }
            for (const std::uint32_t index : { 0u, 1u, 2u, 0u, 2u, 3u })
            {
                builder.add_index(first_vertex + index);
            }
            first_vertex += 4;
        }

        scene::mesh_data make_unit_cube()
        {
            scene::mesh_builder builder{ scene::mesh_topology::triangles };
            std::uint32_t first_vertex = 0;
            for (int axis = 0; axis < 3; ++axis)
            {
                for (const float side : { -1.0f, 1.0f })
                {
                    Eigen::Vector3f normal = Eigen::Vector3f::Zero();
                    normal[axis] = side;
                    // u × v = normal, so the quad winds counter-clockwise seen from outside
                    Eigen::Vector3f u = Eigen::Vector3f::Zero();
                    u[(axis + 1) % 3] = 1.0f;
                    const Eigen::Vector3f v = normal.cross(u);
                    add_quad(builder, first_vertex, 0.5f * (normal - u - v), u, v, normal);
                }
            }
            return build(std::move(builder));
        }

        scene::mesh_data make_sphere(const float radius)
        {
            constexpr std::uint32_t stacks = 16;
            constexpr std::uint32_t slices = 32;
            scene::mesh_builder builder{ scene::mesh_topology::triangles };
            for (std::uint32_t stack = 0; stack <= stacks; ++stack)
            {
                const float polar = std::numbers::pi_v<float> * static_cast<float>(stack) / stacks;
                for (std::uint32_t slice = 0; slice <= slices; ++slice)
                {
                    const float azimuth = 2.0f * std::numbers::pi_v<float> * static_cast<float>(slice) / slices;
                    const Eigen::Vector3f normal{ std::sin(polar) * std::cos(azimuth),
                                                  std::cos(polar),
                                                  std::sin(polar) * std::sin(azimuth) };
                    builder.add_vertex(radius * normal)
                        .normal(normal)
                        .uv({ static_cast<float>(slice) / slices, static_cast<float>(stack) / stacks });
                }
            }
            for (std::uint32_t stack = 0; stack < stacks; ++stack)
            {
                for (std::uint32_t slice = 0; slice < slices; ++slice)
                {
                    const std::uint32_t top = stack * (slices + 1) + slice;
                    const std::uint32_t bottom = top + slices + 1;
                    for (const std::uint32_t index : { top, top + 1, bottom, top + 1, bottom + 1, bottom })
                    {
                        builder.add_index(index);
                    }
                }
            }
            return build(std::move(builder));
        }

        scene::transform at(const Eigen::Vector3f& position)
        {
            scene::transform local;
            local.position = position;
            return local;
        }

        // Completes from poll_completions(), which the test drives
        struct image_receiver
        {
            using receiver_concept = stdexec::receiver_tag;

            void set_value(graphics::readback_image value) noexcept
            {
                image->emplace(std::move(value));
            }

            void set_error(avis::error) noexcept {}
            void set_stopped() noexcept {}

            stdexec::env<> get_env() const noexcept
            {
                return {};
            }

            std::optional<graphics::readback_image>* image;
        };
    } // namespace

    rgba8 rgba8_image::at(const std::uint32_t x, const std::uint32_t y) const
    {
        return pixels.at(static_cast<std::size_t>(y) * width + x);
    }

    std::size_t rgba8_image::count_different(const rgba8 value) const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(pixels, [&](const rgba8& pixel) { return pixel != value; }));
    }

    renderer_fixture::renderer_fixture(const renderer_fixture_options& options) :
        graphics{ options.graph },
        settings{ options.settings },
        output_size{ options.output_size }
    {
        uploads = options.staging_ring_size > 0 ?
                      make_test_upload_queue(graphics.hardware_interface, options.staging_ring_size) :
                      make_test_upload_queue(graphics.hardware_interface);
        readbacks = make_test_readback_queue(graphics.hardware_interface);

        auto library = threading::sync_wait_result(
            rendering::load_renderer_shaders(std::filesystem::path{ AVIS_TEST_RENDERER_SHADER_DIRECTORY }));
        REQUIRE(library.has_value());
        shaders.emplace(std::move(*library));

        create_renderer();
        output = make_output(output_size);
    }

    renderer_fixture::~renderer_fixture()
    {
        graphics.hardware_interface->wait_idle();
        renderer.reset();
        rhi::resources resources{ *graphics.hardware_interface };
        for (const rhi::texture_handle texture : outputs_)
        {
            resources.destroy(texture);
        }
        for (const scene::mesh_handle mesh : meshes_)
        {
            assets.release(mesh);
        }
        for (const scene::material_handle material : materials_)
        {
            assets.release(material);
        }
    }

    void renderer_fixture::create_renderer()
    {
        // A fresh registry each time: the renderer registers its standard signature, once per registry
        pipeline_cache.reset();
        root_signatures =
            std::make_unique<graphics::root_signature_registry>(rhi::pipelines{ *graphics.hardware_interface });
        pipeline_cache = std::make_unique<graphics::pipeline_cache>(
            rhi::pipelines{ *graphics.hardware_interface }, *root_signatures);

        auto created = rendering::scene_renderer::create(
            rendering::renderer_dependencies{
                .resources = rhi::resources{ *graphics.hardware_interface },
                .pipelines = rhi::pipelines{ *graphics.hardware_interface },
                .synchronization = rhi::synchronization{ *graphics.hardware_interface },
                .graph = graphics.graph,
                .pool = graphics.pool,
                .frames = *graphics.frames,
                .profiler = graphics.profiler.get(),
                .pipeline_cache = *pipeline_cache,
                .root_signatures = *root_signatures,
                .uploads = *uploads,
            },
            settings,
            *shaders);
        REQUIRE(created.has_value());
        renderer = std::move(*created);
    }

    void renderer_fixture::destroy_renderer()
    {
        // The first wait lets the frames finish; the second runs the deferred destruction of what the renderer released
        graphics.hardware_interface->wait_idle();
        renderer.reset();
        graphics.hardware_interface->wait_idle();
    }

    scene::entity renderer_fixture::add_entity(const std::string& name, const scene::transform& local)
    {
        const result<scene::entity> created = world.create_entity(
            { .id = next_id_++, .name = name, .category = scene::entity_category::object, .local = local });
        REQUIRE(created.has_value());
        return *created;
    }

    scene::entity renderer_fixture::add_drawn(
        const std::string& name,
        const scene::transform& local,
        scene::mesh_data mesh,
        const scene::material_description& material)
    {
        const result<scene::mesh_handle> mesh_handle = assets.create_mesh(std::move(mesh));
        REQUIRE(mesh_handle.has_value());
        meshes_.push_back(*mesh_handle);
        const result<scene::material_handle> material_handle = assets.create_material(material);
        REQUIRE(material_handle.has_value());
        materials_.push_back(*material_handle);

        const scene::entity entity = add_entity(name, local);
        world.registry().emplace<scene::components::mesh>(entity, scene::components::mesh{ .asset = *mesh_handle });
        world.registry().emplace<scene::components::material>(
            entity, scene::components::material{ .materials = { *material_handle } });
        return entity;
    }

    scene::entity
        renderer_fixture::add_unit_cube(const Eigen::Vector3f& position, const scene::material_description& material)
    {
        return add_drawn("cube", at(position), make_unit_cube(), material);
    }

    scene::entity renderer_fixture::add_sphere(
        const Eigen::Vector3f& position, const float radius, const scene::material_description& material)
    {
        return add_drawn("sphere", at(position), make_sphere(radius), material);
    }

    scene::entity renderer_fixture::add_ground_plane(const float size, const scene::material_description& material)
    {
        scene::mesh_builder builder{ scene::mesh_topology::triangles };
        std::uint32_t first_vertex = 0;
        // u × v = +Y
        add_quad(
            builder,
            first_vertex,
            { -0.5f * size, 0.0f, 0.5f * size },
            { size, 0.0f, 0.0f },
            { 0.0f, 0.0f, -size },
            Eigen::Vector3f::UnitY());
        return add_drawn("ground", {}, build(std::move(builder)), material);
    }

    scene::entity renderer_fixture::add_sun(const Eigen::Vector3f& direction, const float illuminance)
    {
        // A light shines along its −Z axis
        scene::transform local;
        local.rotation = Eigen::Quaternionf::FromTwoVectors(-Eigen::Vector3f::UnitZ(), direction.normalized());
        const scene::entity entity = add_entity("sun", local);
        world.registry().emplace<scene::components::light>(
            entity,
            scene::components::light{
                .kind = scene::light_kind::directional, .intensity = illuminance, .casts_shadows = true });

        scene::environment_settings environment = world.environment();
        environment.sun_direction = direction.normalized();
        environment.sun_illuminance = illuminance;
        world.set_environment(environment);
        return entity;
    }

    scene::entity
        renderer_fixture::add_point_light(const Eigen::Vector3f& position, const float intensity, const float range)
    {
        const scene::entity entity = add_entity("point light", at(position));
        world.registry().emplace<scene::components::light>(
            entity,
            scene::components::light{ .kind = scene::light_kind::point, .intensity = intensity, .range = range });
        return entity;
    }

    scene::entity renderer_fixture::add_unlit_quad(
        const Eigen::Vector3f& position, const float width, const float height, const scene::color color)
    {
        scene::mesh_builder builder{ scene::mesh_topology::triangles };
        std::uint32_t first_vertex = 0;
        add_quad(
            builder,
            first_vertex,
            { -0.5f * width, -0.5f * height, 0.0f },
            { width, 0.0f, 0.0f },
            { 0.0f, height, 0.0f },
            Eigen::Vector3f::UnitZ());
        scene::material_description material;
        material.base_color_factor = color;
        material.shading = scene::shading_model::unlit;
        return add_drawn("unlit quad", at(position), build(std::move(builder)), material);
    }

    void renderer_fixture::publish()
    {
        world.update_world_transforms();
        scene::snapshot& target = snapshots.begin_write();
        world.make_snapshot(target);
        target.anchor = anchor;
        target.time_previous = time_previous;
        target.time_current = time_current;
        snapshots.publish();
        world.end_tick();
    }

    rhi::texture_handle renderer_fixture::make_output(const graphics::texture_extent size)
    {
        auto texture = rhi::resources{ *graphics.hardware_interface }.create_texture(
            { .width = size.width,
              .height = size.height,
              .format = rhi::format::rgba8_unorm,
              .usage = rhi::texture_usage::copy_destination | rhi::texture_usage::copy_source,
              .initial_layout = rhi::texture_layout::common,
              .debug_name = "renderer test output" });
        REQUIRE(texture.has_value());
        outputs_.push_back(*texture);
        return *texture;
    }

    rendering::view_description
        renderer_fixture::view(const std::string& name, const std::optional<rhi::texture_handle> target)
    {
        const rhi::texture_handle texture = target.value_or(output);
        const rhi::texture_description& description =
            rhi::resources{ *graphics.hardware_interface }.description(texture);
        rendering::view_description made;
        made.name = name;
        made.size = { description.width, description.height };
        made.output = rendering::output_target{ .texture = texture };
        return made;
    }

    result<graphics::submitted_frame>
        renderer_fixture::render_frame(const std::span<const rendering::view_description> views)
    {
        if (snapshots.publish_sequence() == 0)
        {
            publish();
        }

        graphics::frame_token frame = graphics.begin_frame();
        renderer->retire(frame.completed_index());

        result<graphics::submitted_frame> submitted;
        {
            const scene::snapshot_reference snapshot = snapshots.acquire();
            REQUIRE(static_cast<bool>(snapshot));
            submitted = threading::sync_wait_result(
                renderer->render(frame, *snapshot, views, wall_now.value_or(std::chrono::steady_clock::now())));
        }
        if (submitted)
        {
            graphics.wait(*submitted);
            graphics.frames->end_frame(std::move(frame), submitted->last_submissions());
        }
        // Otherwise the token's destructor ends the frame without submissions
        return submitted;
    }

    std::vector<rgba8_image>
        renderer_fixture::render_and_read(const std::span<const rendering::view_description> views, const int frames)
    {
        for (int frame = 0; frame < frames; ++frame)
        {
            const result<graphics::submitted_frame> submitted = render_frame(views);
            if (!submitted)
            {
                FAIL(std::format("frame {} failed: {}", frame, submitted.error()));
            }
        }

        std::vector<rgba8_image> images;
        images.reserve(views.size());
        for (const rendering::view_description& view : views)
        {
            images.push_back(view.output ? read_output(view.output->texture) : rgba8_image{});
        }
        return images;
    }

    graphics::readback_image
        renderer_fixture::read_subresource(const rhi::texture_handle texture, const rhi::subresource subresource)
    {
        std::optional<graphics::readback_image> image;
        auto operation = stdexec::connect(readbacks->read_texture(texture, subresource), image_receiver{ &image });
        stdexec::start(operation);
        graphics.hardware_interface.wait_for(
            rhi::queue<rhi::queue_type::graphics>{ *graphics.hardware_interface }.last_submitted());
        REQUIRE(image.has_value());
        return std::move(*image);
    }

    rgba8_image renderer_fixture::read_output(const rhi::texture_handle texture)
    {
        const std::optional<graphics::readback_image> image = read_subresource(texture);
        REQUIRE(image->format() == rhi::format::rgba8_unorm);

        rgba8_image read{ .width = image->width(), .height = image->height() };
        read.pixels.resize(static_cast<std::size_t>(read.width) * read.height);
        for (std::uint32_t y = 0; y < read.height; ++y)
        {
            std::memcpy(
                read.pixels.data() + static_cast<std::size_t>(y) * read.width,
                image->bytes().data() + static_cast<std::size_t>(y) * image->row_pitch(),
                static_cast<std::size_t>(read.width) * 4);
        }
        return read;
    }
} // namespace avis::tests
