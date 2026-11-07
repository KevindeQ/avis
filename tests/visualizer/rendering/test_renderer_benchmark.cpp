// The end-to-end frame of 11 §15 (docs/tasks/11-renderer.md §11.23): 20M progressive points, 500 instanced meshes, a
// sun and 64 point lights at 1920 x 1080, on the renderer fixture with the debug layer but without GPU-based
// validation. Not run by default: the numbers are recorded in the task's As implemented section.

#include "helpers/renderer_fixture.h"
#include "rendering/scene_renderer.h"
#include "rendering/view.h"

#include "avis/graphics/graph_debug_information.h"
#include "avis/scene/assets/mesh_builder.h"
#include "avis/scene/components/rendering.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <numbers>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;
using avis::tests::renderer_fixture;
using avis::tests::renderer_fixture_options;

namespace
{
    constexpr std::uint32_t point_clouds = 20;
    constexpr std::uint32_t points_per_cloud = 1'000'000;
    constexpr std::uint32_t mesh_instances = 500;
    constexpr std::uint32_t point_lights = 64;

    scene::mesh_data built(scene::mesh_builder builder)
    {
        result<scene::mesh_data> mesh = std::move(builder).build();
        REQUIRE(mesh.has_value());
        return std::move(*mesh);
    }

    // A 30 m x 30 m x 3 m slab of random points, like a lidar sweep's ground and objects, in random order
    scene::mesh_data make_point_cloud(std::mt19937& random)
    {
        scene::mesh_builder builder{ scene::mesh_topology::points, true };
        std::uniform_real_distribution<float> horizontal{ -15.0f, 15.0f };
        std::uniform_real_distribution<float> height{ 0.0f, 3.0f };
        for (std::uint32_t index = 0; index < points_per_cloud; ++index)
        {
            builder.add_vertex({ horizontal(random), height(random), horizontal(random) });
        }
        return built(std::move(builder));
    }

    scene::mesh_data make_box()
    {
        scene::mesh_builder builder{ scene::mesh_topology::triangles };
        std::uint32_t first = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            for (const float side : { -1.0f, 1.0f })
            {
                Eigen::Vector3f normal = Eigen::Vector3f::Zero();
                normal[axis] = side;
                Eigen::Vector3f u = Eigen::Vector3f::Zero();
                u[(axis + 1) % 3] = 1.0f;
                const Eigen::Vector3f v = normal.cross(u);
                const Eigen::Vector3f origin = 0.5f * (normal - u - v);
                for (const Eigen::Vector3f& corner : { origin,
                                                       Eigen::Vector3f(origin + u),
                                                       Eigen::Vector3f(origin + u + v),
                                                       Eigen::Vector3f(origin + v) })
                {
                    builder.add_vertex(corner).normal(normal);
                }
                for (const std::uint32_t index : { 0u, 1u, 2u, 0u, 2u, 3u })
                {
                    builder.add_index(first + index);
                }
                first += 4;
            }
        }
        return built(std::move(builder));
    }

    scene::mesh_data make_ball()
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
                builder.add_vertex(0.5f * normal).normal(normal);
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
        return built(std::move(builder));
    }

    // The scene's own meshes and materials: shared by several entities, so the instances batch
    struct shared_assets
    {
        renderer_fixture& fixture;
        std::vector<scene::mesh_handle> meshes;
        std::vector<scene::material_handle> materials;
        scene::object_id next_id = 1'000'000; // clear of the fixture's ids

        scene::mesh_handle mesh(scene::mesh_data data)
        {
            const result<scene::mesh_handle> handle = fixture.assets.create_mesh(std::move(data));
            REQUIRE(handle.has_value());
            meshes.push_back(*handle);
            return *handle;
        }

        scene::material_handle material(const scene::material_description& description)
        {
            const result<scene::material_handle> handle = fixture.assets.create_material(description);
            REQUIRE(handle.has_value());
            materials.push_back(*handle);
            return *handle;
        }

        void
            add(const std::string& name,
                const Eigen::Vector3f& position,
                const scene::mesh_handle mesh_handle,
                const scene::material_handle material_handle)
        {
            scene::transform local;
            local.position = position;
            const result<scene::entity> entity = fixture.world.create_entity(
                { .id = next_id++, .name = name, .category = scene::entity_category::object, .local = local });
            REQUIRE(entity.has_value());
            fixture.world.registry().emplace<scene::components::mesh>(
                *entity, scene::components::mesh{ .asset = mesh_handle });
            fixture.world.registry().emplace<scene::components::material>(
                *entity, scene::components::material{ .materials = { material_handle } });
        }

        // After the GPU is idle; the scene's own references go with the fixture
        void release()
        {
            for (const scene::mesh_handle handle : meshes)
            {
                fixture.assets.release(handle);
            }
            for (const scene::material_handle handle : materials)
            {
                fixture.assets.release(handle);
            }
        }
    };

    struct frame_timings
    {
        double cpu_build_milliseconds = 0.0; // render(): residency, extraction, draw lists, graph build and compile
        double wall_milliseconds = 0.0; // render() to the GPU's completion, recording included
        double gpu_sum_milliseconds = 0.0; // every pass's GPU time (async compute counted as if serial)
        double gpu_graphics_milliseconds = 0.0; // the graphics queue's passes only
        double shadow_milliseconds = 0.0;
        double gtao_milliseconds = 0.0;
        bool timed = false;
    };

    frame_timings render_timed(renderer_fixture& fixture, const std::vector<view_description>& views)
    {
        frame_timings timings;
        graphics::frame_token frame = fixture.graphics.begin_frame();
        fixture.renderer->retire(frame.completed_index());

        const auto start = std::chrono::steady_clock::now();
        result<graphics::submitted_frame> submitted;
        {
            const scene::snapshot_reference snapshot = fixture.snapshots.acquire();
            REQUIRE(static_cast<bool>(snapshot));
            auto sender = fixture.renderer->render(frame, *snapshot, views, std::chrono::steady_clock::now());
            timings.cpu_build_milliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            submitted = threading::sync_wait_result(std::move(sender));
        }
        REQUIRE(submitted.has_value());
        fixture.graphics.wait(*submitted);
        timings.wall_milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        fixture.graphics.frames->end_frame(std::move(frame), submitted->last_submissions());

        for (const graphics::pass_debug_information& pass : fixture.graphics.graph.debug_information().passes)
        {
            if (pass.culled || !pass.gpu_milliseconds)
            {
                continue;
            }
            timings.timed = true;
            timings.gpu_sum_milliseconds += *pass.gpu_milliseconds;
            if (pass.queue == rhi::queue_type::graphics)
            {
                timings.gpu_graphics_milliseconds += *pass.gpu_milliseconds;
            }
            if (pass.name.find("shadow_cascades") != std::string_view::npos)
            {
                timings.shadow_milliseconds += *pass.gpu_milliseconds;
            }
            if (pass.name.ends_with("gtao") || pass.name.find("gtao_denoise_") != std::string_view::npos)
            {
                timings.gtao_milliseconds += *pass.gpu_milliseconds;
            }
        }
        return timings;
    }

    double median(std::vector<double> values)
    {
        REQUIRE_FALSE(values.empty());
        std::ranges::sort(values);
        return values[values.size() / 2];
    }
} // namespace

TEST_CASE("the 11 §15 frame renders in under 12 ms of GPU time at 1920 x 1080", "[renderer][!benchmark][.]")
{
    renderer_fixture_options options;
    options.graph.gpu_based_validation = false;
    options.graph.profiler = true;
    options.graph.timestamps_per_frame = 1'024;
    options.output_size = { 1920, 1080 };
    options.settings = render_settings{}; // 11 §14's defaults: a 768 MiB arena, a 20M vertex budget, auto exposure
    options.staging_ring_size = std::size_t{ 64 } << 20; // five clouds per frame
    renderer_fixture fixture{ options };
    shared_assets assets{ fixture };

    std::mt19937 random{ 1115 };
    fixture.add_ground_plane(400.0f);
    fixture.add_sun(Eigen::Vector3f(-0.4f, -0.7f, -0.5f));

    // The point clouds in a 5 x 4 grid in front of the camera
    scene::material_description points;
    points.base_color_factor = { 0.6f, 0.6f, 0.6f, 1.0f };
    points.point_size = 0.04f;
    const scene::material_handle point_material = assets.material(points);
    for (std::uint32_t cloud = 0; cloud < point_clouds; ++cloud)
    {
        const Eigen::Vector3f position{ -60.0f + 30.0f * static_cast<float>(cloud % 5),
                                        0.0f,
                                        -25.0f - 30.0f * static_cast<float>(cloud / 5) };
        assets.add(std::format("cloud {}", cloud), position, assets.mesh(make_point_cloud(random)), point_material);
    }

    // 500 instances of two meshes and two materials
    scene::material_description red;
    red.base_color_factor = { 0.8f, 0.2f, 0.1f, 1.0f };
    red.roughness_factor = 0.5f;
    scene::material_description blue;
    blue.base_color_factor = { 0.1f, 0.3f, 0.8f, 1.0f };
    blue.metallic_factor = 1.0f;
    blue.roughness_factor = 0.3f;
    const scene::mesh_handle box = assets.mesh(make_box());
    const scene::mesh_handle ball = assets.mesh(make_ball());
    const scene::material_handle red_material = assets.material(red);
    const scene::material_handle blue_material = assets.material(blue);
    std::uniform_real_distribution<float> across{ -70.0f, 70.0f };
    std::uniform_real_distribution<float> ahead{ -130.0f, -5.0f };
    for (std::uint32_t index = 0; index < mesh_instances; ++index)
    {
        const bool is_box = index % 2 == 0;
        assets.add(
            is_box ? "box" : "ball",
            { across(random), 0.5f, ahead(random) },
            is_box ? box : ball,
            index % 4 < 2 ? red_material : blue_material);
    }

    std::uniform_real_distribution<float> light_height{ 1.0f, 4.0f };
    for (std::uint32_t index = 0; index < point_lights; ++index)
    {
        fixture.add_point_light({ across(random), light_height(random), ahead(random) }, 2'000.0f, 15.0f);
    }
    fixture.publish();

    // 6 m above the ground, looking 10° down
    view_description main = fixture.view();
    main.camera.world_from_view =
        Eigen::Translation3f(0.0f, 6.0f, 0.0f) *
        Eigen::AngleAxisf(-10.0f * std::numbers::pi_v<float> / 180.0f, Eigen::Vector3f::UnitX());
    const std::vector views{ main };

    // Until every mesh is resident and the frames' timings come back
    int warm_up = 0;
    for (; warm_up < 60; ++warm_up)
    {
        render_timed(fixture, views);
        const renderer_debug_information information = fixture.renderer->debug_information();
        if (information.residency.pending_uploads == 0 && warm_up >= 8)
        {
            break;
        }
    }
    {
        const renderer_debug_information information = fixture.renderer->debug_information();
        REQUIRE(information.residency.pending_uploads == 0);
        REQUIRE(information.extraction.instances == point_clouds + mesh_instances + 1);
    }

    std::vector<double> cpu;
    std::vector<double> wall;
    std::vector<double> gpu_sum;
    std::vector<double> gpu_graphics;
    std::vector<double> shadows;
    std::vector<double> gtao;
    for (int frame = 0; frame < 32; ++frame)
    {
        const frame_timings timings = render_timed(fixture, views);
        cpu.push_back(timings.cpu_build_milliseconds);
        wall.push_back(timings.wall_milliseconds);
        if (timings.timed)
        {
            gpu_sum.push_back(timings.gpu_sum_milliseconds);
            gpu_graphics.push_back(timings.gpu_graphics_milliseconds);
            shadows.push_back(timings.shadow_milliseconds);
            gtao.push_back(timings.gtao_milliseconds);
        }
    }

    const renderer_debug_information information = fixture.renderer->debug_information();
    REQUIRE(information.progressive.size() == 1);
    REQUIRE(information.draw_lists.size() == 1);
    const progressive_lod_statistics& lod = information.progressive.front();
    std::size_t draws = 0;
    std::size_t pipeline_changes = 0;
    for (const pass_draw_statistics& pass : information.passes)
    {
        draws += pass.walk.draws;
        pipeline_changes += pass.walk.pipeline_changes;
    }

    const double frame_gpu = median(gpu_sum);
    WARN(
        std::format(
            "warm-up frames {}; medians over 32 frames: GPU {:.2f} ms (sum of passes; graphics queue {:.2f} ms), "
            "shadow_cascades {:.2f} ms, GTAO {:.2f} ms, CPU build {:.2f} ms, wall {:.2f} ms. Progressive: k {:.3f}, "
            "camera vertices {}, shadow vertices {}, whole {}. Draws {}, pipeline changes {}, instances {}.",
            warm_up,
            frame_gpu,
            median(gpu_graphics),
            median(shadows),
            median(gtao),
            median(cpu),
            median(wall),
            lod.vertices_per_pixel,
            lod.camera_vertices,
            lod.shadow_vertices,
            lod.full_vertices,
            draws,
            pipeline_changes,
            information.extraction.instances));

    // The last frame's passes above 0.1 ms, slowest first
    std::vector<std::pair<double, std::string>> slowest;
    for (const graphics::pass_debug_information& pass : fixture.graphics.graph.debug_information().passes)
    {
        if (!pass.culled && pass.gpu_milliseconds && *pass.gpu_milliseconds >= 0.1)
        {
            slowest.emplace_back(*pass.gpu_milliseconds, std::string{ pass.name });
        }
    }
    std::ranges::sort(slowest, std::greater{});
    std::string breakdown;
    for (const auto& [milliseconds, name] : slowest)
    {
        breakdown += std::format("{} {:.2f} ms; ", name, milliseconds);
    }
    WARN("passes: " << breakdown);

    fixture.graphics.hardware_interface->wait_idle();
    assets.release();

    CHECK(frame_gpu < 12.0);
    CHECK(median(shadows) < 2.0);
    CHECK(median(gtao) < 0.8);
}
