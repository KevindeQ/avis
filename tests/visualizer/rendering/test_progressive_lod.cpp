// Progressive meshes (docs/tasks/11-renderer.md §11.18): the per-view LOD of progressive_lod.h (the budget, the rate
// limit, the shadow scale, the projected area) as unit tests, and a 1M-point mesh drawn under a budget on the renderer
// fixture.

#include "helpers/renderer_fixture.h"
#include "rendering/progressive_lod.h"
#include "rendering/view.h"

#include "avis/scene/assets/mesh_builder.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <utility>
#include <vector>

using namespace avis;
using namespace avis::visualizer::rendering;

namespace
{
    std::vector<progressive_vertex_counts> update(
        progressive_lod& lod,
        const std::vector<progressive_instance>& instances,
        const progressive_lod_parameters& parameters,
        progressive_lod_statistics* statistics = nullptr)
    {
        std::vector<progressive_vertex_counts> counts(instances.size());
        const progressive_lod_statistics computed = lod.update(instances, parameters, counts);
        if (statistics != nullptr)
        {
            *statistics = computed;
        }
        return counts;
    }

    std::uint64_t total_of(
        const std::vector<progressive_instance>& instances, const std::vector<progressive_vertex_counts>& counts)
    {
        std::uint64_t total = 0;
        for (std::size_t index = 0; index < instances.size(); ++index)
        {
            total += (instances[index].camera ? counts[index].camera : 0) +
                     std::uint64_t{ instances[index].shadow_viewports } * counts[index].shadow;
        }
        return total;
    }
} // namespace

TEST_CASE("the progressive total stays under the budget, and a bigger area gets more vertices", "[renderer][unit]")
{
    std::mt19937 random{ 1811 };
    std::uniform_int_distribution<std::uint32_t> vertex_count{ 1'000, 1'000'000 };
    std::uniform_real_distribution<float> log_area{ 1.0f, 6.0f };
    std::uniform_int_distribution<std::uint32_t> shadows{ 0, 3 };

    std::vector<progressive_instance> instances;
    for (std::uint64_t id = 1; id <= 100; ++id)
    {
        instances.push_back(
            { .object_id = id,
              .vertex_count = vertex_count(random),
              .projected_area = std::pow(10.0f, log_area(random)),
              .camera = id % 7 != 0, // a few cast shadows without being in the camera
              .shadow_viewports = shadows(random) });
    }

    for (const std::uint64_t budget : { 100'000ull, 1'000'000ull, 5'000'000ull })
    {
        CAPTURE(budget);
        progressive_lod lod;
        progressive_lod_statistics statistics;
        const std::vector<progressive_vertex_counts> counts =
            update(lod, instances, { .vertex_budget = budget }, &statistics);
        const std::uint64_t total = total_of(instances, counts);
        CAPTURE(total, statistics.full_vertices, statistics.vertices_per_pixel);
        CHECK(statistics.vertices() == total);
        CHECK(total <= budget);
        CHECK_FALSE(statistics.minimums_scaled);
        // The budget is the only limit: it's used up to the rounding of 100 instances
        CHECK(total >= std::min(statistics.full_vertices, budget) - 1'000);
        CHECK(total < statistics.full_vertices);
        CHECK(lod.size() == instances.size());
    }

    // The same mesh at random areas: more area, more vertices
    std::vector<progressive_instance> same_mesh;
    for (std::uint64_t id = 1; id <= 100; ++id)
    {
        same_mesh.push_back(
            { .object_id = id,
              .vertex_count = 1'000'000,
              .projected_area = std::pow(10.0f, log_area(random)),
              .camera = true });
    }
    progressive_lod lod;
    const std::vector<progressive_vertex_counts> counts = update(lod, same_mesh, { .vertex_budget = 2'000'000 });
    CHECK(total_of(same_mesh, counts) <= 2'000'000);
    std::vector<std::size_t> order(same_mesh.size());
    for (std::size_t index = 0; index < order.size(); ++index)
    {
        order[index] = index;
    }
    std::ranges::sort(order, {}, [&](const std::size_t index) { return same_mesh[index].projected_area; });
    for (std::size_t index = 1; index < order.size(); ++index)
    {
        CHECK(counts[order[index]].camera >= counts[order[index - 1]].camera);
    }
    CHECK(counts[order.back()].camera > counts[order.front()].camera);
}

TEST_CASE("progressive instances get the minimum, k times their area, or the whole mesh", "[renderer][unit]")
{
    const std::vector<progressive_instance> instances{
        { .object_id = 1, .vertex_count = 1'000'000, .projected_area = 10.0f, .camera = true }, // the minimum
        { .object_id = 2, .vertex_count = 100, .projected_area = 1'000.0f, .camera = true }, // fewer than the minimum
        { .object_id = 3, .vertex_count = 1'000'000, .projected_area = 10'000.0f, .camera = true }, // k · area
        { .object_id = 4, .vertex_count = 5'000, .projected_area = 100'000.0f, .camera = true }, // whole
    };
    progressive_lod lod;
    progressive_lod_statistics statistics;
    std::vector<progressive_vertex_counts> counts = update(lod, instances, { .vertex_budget = 30'000 }, &statistics);
    CHECK(counts[0].camera == 256);
    CHECK(counts[1].camera == 100);
    CHECK(counts[2].camera == 30'000 - 256 - 100 - 5'000);
    CHECK(counts[3].camera == 5'000);
    CHECK(statistics.vertices() == 30'000);
    CHECK(statistics.vertices_per_pixel == Catch::Approx(2.4644).margin(1e-4));

    // Under the budget, everything is whole
    lod.reset();
    counts = update(lod, instances, { .vertex_budget = 100'000'000 }, &statistics);
    CHECK(counts[0].camera == 1'000'000);
    CHECK(counts[2].camera == 1'000'000);
    CHECK(statistics.vertices() == statistics.full_vertices);
    CHECK(std::isinf(statistics.vertices_per_pixel));

    // An empty mesh draws nothing and isn't remembered
    const std::vector<progressive_vertex_counts> empty =
        update(lod, { { .object_id = 9, .vertex_count = 0, .projected_area = 100.0f, .camera = true } }, {});
    CHECK(empty[0] == progressive_vertex_counts{});
    CHECK(lod.size() == 0);
}

TEST_CASE("the progressive rate limit holds when the camera jumps", "[renderer][unit]")
{
    // k = 2 shares the budget as 2,000 + 800,000
    const progressive_lod_parameters parameters{ .vertex_budget = 802'000 };
    progressive_lod lod;
    std::vector<progressive_instance> instances{
        { .object_id = 7, .vertex_count = 2'000'000, .projected_area = 1'000.0f, .camera = true },
        { .object_id = 8, .vertex_count = 2'000'000, .projected_area = 400'000.0f, .camera = true },
    };
    std::vector<progressive_vertex_counts> previous = update(lod, instances, parameters);
    REQUIRE(previous[0].camera == 2'000);
    REQUIRE(previous[1].camera == 800'000);

    // The camera jumps: object 7 gets 200× its area, object 8 a 1/400th of it. Settled, they share the budget as
    // 200:1, about 798,000 and 3,990.
    instances[0].projected_area = 200'000.0f;
    instances[1].projected_area = 1'000.0f;
    int frames = 0;
    progressive_lod_statistics statistics;
    while (frames < 100)
    {
        const std::vector<progressive_vertex_counts> counts = update(lod, instances, parameters, &statistics);
        ++frames;
        CHECK(statistics.vertices() <= parameters.vertex_budget);
        for (std::size_t index = 0; index < instances.size(); ++index)
        {
            CAPTURE(frames, index, previous[index].camera, counts[index].camera);
            CHECK(counts[index].camera <= static_cast<std::uint32_t>(std::floor(previous[index].camera * 1.25)));
            CHECK(counts[index].camera >= static_cast<std::uint32_t>(std::ceil(previous[index].camera * 0.75)));
        }
        const bool settled = counts == previous;
        previous = counts;
        if (settled)
        {
            break;
        }
    }
    // ln(399) / ln(1.25) ≈ 27 frames up at best; the last thousands come slower, as object 8's 25% steps down free
    // less of the budget each frame
    CAPTURE(previous[0].camera, previous[1].camera);
    CHECK(frames >= 25);
    CHECK(frames <= 60);
    CHECK(previous[0].camera >= 797'000);
    CHECK(previous[1].camera <= 4'000);
    CHECK(statistics.rate_limited == 0);
    CHECK(statistics.vertices() >= parameters.vertex_budget - 2);

    // A history_invalid instance starts free
    const progressive_lod_parameters roomy{ .vertex_budget = 100'000'000 };
    instances[1].history_invalid = true;
    std::vector<progressive_vertex_counts> counts = update(lod, instances, roomy, &statistics);
    CHECK(counts[1].camera == 2'000'000);
    CHECK(counts[0].camera == static_cast<std::uint32_t>(std::floor(previous[0].camera * 1.25)));
    CHECK(statistics.rate_limited == 1);

    // So does every instance after a reset
    instances[1].history_invalid = false;
    lod.reset();
    counts = update(lod, instances, roomy, &statistics);
    CHECK(counts[0].camera == 2'000'000);
    CHECK(statistics.rate_limited == 0);

    // And an object the view didn't draw in its previous frame: object 7 drew 2M two frames ago, which would keep it
    // at 1.5M or more
    update(lod, { instances[1] }, roomy);
    counts = update(lod, { instances[0] }, { .vertex_budget = 1'000 }, &statistics);
    CHECK(counts[0].camera == 1'000);
    CHECK_FALSE(statistics.minimums_scaled);
}

TEST_CASE("the progressive budget wins over the rate limit and the minimums", "[renderer][unit]")
{
    progressive_lod lod;
    std::vector<progressive_instance> instances{
        { .object_id = 1, .vertex_count = 1'000'000, .projected_area = 100'000.0f, .camera = true },
    };
    REQUIRE(update(lod, instances, { .vertex_budget = 200'000 })[0].camera == 200'000);

    // The budget drops to a tenth: the rate limit would keep 150k
    progressive_lod_statistics statistics;
    std::vector<progressive_vertex_counts> counts = update(lod, instances, { .vertex_budget = 20'000 }, &statistics);
    CHECK(counts[0].camera == 20'000);
    CHECK(statistics.minimums_scaled);

    // 1,000 instances whose minimums alone are 256k
    instances.clear();
    for (std::uint64_t id = 1; id <= 1'000; ++id)
    {
        instances.push_back(
            { .object_id = id,
              .vertex_count = 10'000,
              .projected_area = 100.0f,
              .camera = true,
              .shadow_viewports = 1 });
    }
    progressive_lod fresh;
    counts = update(fresh, instances, { .vertex_budget = 100'000 }, &statistics);
    CHECK(statistics.minimums_scaled);
    CHECK(total_of(instances, counts) <= 100'000);
    CHECK(counts[0].camera >= 1);
}

TEST_CASE("progressive shadow viewports get the scale times n", "[renderer][unit]")
{
    progressive_lod lod;
    const std::vector<progressive_instance> instances{
        { .object_id = 1,
          .vertex_count = 1'000'000,
          .projected_area = 50'000.0f,
          .camera = true,
          .shadow_viewports = 2 },
        { .object_id = 2, .vertex_count = 1'000'000, .projected_area = 7'000.0f, .shadow_viewports = 1 },
        { .object_id = 3, .vertex_count = 1, .projected_area = 7'000.0f, .camera = true, .shadow_viewports = 1 },
    };
    for (const float scale : { 0.25f, 0.5f, 1.0f })
    {
        CAPTURE(scale);
        progressive_lod_statistics statistics;
        const std::vector<progressive_vertex_counts> counts =
            update(lod, instances, { .vertex_budget = 100'000'000, .shadow_lod_scale = scale }, &statistics);
        CHECK(counts[0].camera == 1'000'000);
        CHECK(counts[0].shadow == static_cast<std::uint32_t>(scale * 1'000'000));
        CHECK(counts[1].shadow == static_cast<std::uint32_t>(scale * 1'000'000));
        CHECK(counts[2].shadow == 1); // at least one vertex
        CHECK(statistics.camera_vertices == 1'000'001);
        CHECK(statistics.shadow_vertices == 2 * counts[0].shadow + counts[1].shadow + 1);
        lod.reset();
    }
    CHECK(shadow_vertex_count(1'001, 0.25f) == 250);
    CHECK(shadow_vertex_count(3, 0.25f) == 1);

    // Under a budget, the shadow vertices count against it
    progressive_lod_statistics statistics;
    const std::vector<progressive_vertex_counts> counts =
        update(lod, instances, { .vertex_budget = 30'000 }, &statistics);
    CHECK(statistics.vertices() <= 30'000);
    CHECK(statistics.vertices() >= 29'990);
    CHECK(counts[0].camera > 10'000);
    CHECK(counts[0].shadow == shadow_vertex_count(counts[0].camera, 0.25f));
    CHECK(counts[0].shadow == counts[0].camera / 4);
    CHECK(counts[1].shadow == counts[1].camera / 4);
}

TEST_CASE("the projected area of a bounding sphere", "[renderer][unit]")
{
    camera viewer; // at the origin, a 60° vertical field of view
    const graphics::texture_extent size{ 1920, 1080 };
    const float focal = 540.0f / std::tan(std::numbers::pi_v<float> / 6.0f);

    const float radius = 1.0f;
    const float distance = 20.0f;
    const float projected = focal * radius / std::sqrt(distance * distance - radius * radius);
    CHECK(
        projected_area({ 0.0f, 0.0f, -distance }, radius, viewer, size) ==
        Catch::Approx(std::numbers::pi_v<float> * projected * projected).epsilon(1e-5));
    // It doesn't depend on the direction, and falls with the square of the distance
    CHECK(
        projected_area({ distance, 0.0f, 0.0f }, radius, viewer, size) ==
        Catch::Approx(projected_area({ 0.0f, 0.0f, -distance }, radius, viewer, size)));
    CHECK(
        projected_area({ 0.0f, 0.0f, -200.0f }, radius, viewer, size) ==
        Catch::Approx(projected_area({ 0.0f, 0.0f, -distance }, radius, viewer, size) / 100.0f).epsilon(1e-2));
    // At most the view, which it is from inside
    CHECK(projected_area({ 0.0f, 0.0f, -1.5f }, radius, viewer, size) == Catch::Approx(1920.0f * 1080.0f));
    CHECK(projected_area({ 0.0f, 0.0f, -0.5f }, radius, viewer, size) == Catch::Approx(1920.0f * 1080.0f));
    CHECK(projected_area({ 0.0f, 0.0f, -5.0f }, 0.0f, viewer, size) == 0.0f);
}

TEST_CASE("a 1M-point progressive mesh draws the budget's vertices", "[renderer]")
{
    tests::renderer_fixture_options options;
    options.graph.gpu_based_validation = false; // 6M vertex shader invocations per frame
    options.output_size = { 512, 288 };
    options.settings.geometry.arena_mib = 64;
    options.staging_ring_size = std::size_t{ 32 } << 20;
    options.settings.exposure.ev100 = -std::log2(1.2f); // a pre-exposure of 1: white points stay white
    tests::renderer_fixture fixture{ options };
    fixture.renderer->set_environment_override(Eigen::Vector3f::Zero());

    // A 4 m cube of random points 4 m in front of the camera, in white unlit points: they fill the view
    scene::mesh_builder builder{ scene::mesh_topology::points, true };
    std::mt19937 random{ 18 };
    std::uniform_real_distribution<float> coordinate{ -2.0f, 2.0f };
    constexpr std::uint32_t point_count = 1'000'000;
    for (std::uint32_t index = 0; index < point_count; ++index)
    {
        builder.add_vertex({ coordinate(random), coordinate(random), coordinate(random) });
    }
    result<scene::mesh_data> mesh = std::move(builder).build();
    REQUIRE(mesh.has_value());
    scene::material_description material;
    material.base_color_factor = { 1.0f, 1.0f, 1.0f, 1.0f };
    material.shading = scene::shading_model::unlit;
    material.point_size = 0.004f;
    fixture.add_mesh("points", { 0.0f, 0.0f, -4.0f }, std::move(*mesh), material);
    fixture.publish();

    const auto lit_pixels = [](const tests::rgba8_image& image)
    {
        std::size_t lit = 0;
        for (std::uint32_t y = 0; y < image.height; ++y)
        {
            for (std::uint32_t x = 0; x < image.width; ++x)
            {
                lit += image.at(x, y)[0] > 0 ? 1 : 0;
            }
        }
        return lit;
    };

    view_description view = fixture.view();
    view.options.taa = false;
    view.options.progressive_vertex_budget = 100'000;
    const tests::rgba8_image budgeted = fixture.render_and_read({ &view, 1 }).front();
    const renderer_debug_information information = fixture.renderer->debug_information();
    REQUIRE(information.progressive_vertices.size() == 1);
    REQUIRE(information.progressive.size() == 1);
    INFO("drawn " << information.progressive_vertices[0] << ", k " << information.progressive[0].vertices_per_pixel);
    CHECK(information.progressive[0].instances == 1);
    CHECK(information.progressive[0].full_vertices == point_count);
    CHECK(information.progressive_vertices[0] >= 99'000);
    CHECK(information.progressive_vertices[0] <= 100'000);
    CHECK(information.progressive[0].camera_vertices == information.progressive_vertices[0]);

    // Lifted (as a screenshot does), the whole mesh is drawn, the rate limit allowing; reset_history lifts that too
    view.options.progressive_vertex_budget = std::uint64_t{ 1 } << 40;
    view.options.reset_history = true;
    const tests::rgba8_image whole = fixture.render_and_read({ &view, 1 }).front();
    CHECK(fixture.renderer->debug_information().progressive_vertices[0] == point_count);
    INFO("lit pixels: budgeted " << lit_pixels(budgeted) << ", whole " << lit_pixels(whole));
    CHECK(lit_pixels(budgeted) > 0);
    CHECK(lit_pixels(budgeted) < lit_pixels(whole));
}
