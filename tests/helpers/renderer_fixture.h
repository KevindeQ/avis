#ifndef AVIS_TESTS_HELPERS_RENDERER_FIXTURE_H
#define AVIS_TESTS_HELPERS_RENDERER_FIXTURE_H

// The scene renderer on the render-graph fixture (docs/tasks/11-renderer.md §11.2): a runtime, the graph and its frame
// scheduler, the renderer created from the built shader directory with the services it needs, an RGBA8 output
// texture, and a scene published to a snapshot buffer. Tests render frames through the scheduler and read the outputs
// back. The debug layer and GPU-based validation are on; the RHI capture fails a test on any validation error.

#include "render_graph_fixture.h"
#include "rendering/render_settings.h"
#include "rendering/scene_renderer.h"
#include "rendering/view.h"
#include "runtime_fixture.h"

#include "avis/graphics/pipeline_cache.h"
#include "avis/graphics/readback_queue.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_library.h"
#include "avis/graphics/upload_queue.h"
#include "avis/rhi/types.h"
#include "avis/runtime.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/assets/material.h"
#include "avis/scene/scene.h"
#include "avis/scene/snapshot.h"
#include "avis/scene/types.h"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace avis::tests
{
    using rgba8 = std::array<std::uint8_t, 4>;

    // A read-back RGBA8 image, tightly packed
    struct rgba8_image
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<rgba8> pixels; // row-major

        rgba8 at(std::uint32_t x, std::uint32_t y) const;
        // Pixels that differ from `value` (0 when the image is uniformly `value`)
        std::size_t count_different(rgba8 value) const;
    };

    // visualizer::rendering::background_color in RGBA8
    inline constexpr rgba8 background_rgba8{ 51, 102, 153, 255 };

    // The defaults of 11 §14, with a 16 MiB geometry arena: the default 768 MiB is sized for the Visualizer's
    // geometry budget, and the tests' scenes are small
    inline visualizer::rendering::render_settings test_render_settings()
    {
        visualizer::rendering::render_settings settings;
        settings.geometry.arena_mib = 16;
        return settings;
    }

    struct renderer_fixture_options
    {
        render_graph_fixture_options graph{ .gpu_based_validation = true };
        visualizer::rendering::render_settings settings = test_render_settings();
        graphics::texture_extent output_size{ 128, 72 };
        std::size_t staging_ring_size = 0; // 0 = the test default (graphics_fixture.h)
    };

    // Members are destroyed in reverse order: the scene and its assets, the renderer, the shader library, the queues
    // and caches, the graph fixture (its RHI last), then the runtime
    class renderer_fixture
    {
    public:
        explicit renderer_fixture(const renderer_fixture_options& options = {});
        ~renderer_fixture(); // waits for the GPU, releases the scene's assets and the outputs
        renderer_fixture(const renderer_fixture&) = delete;
        renderer_fixture& operator=(const renderer_fixture&) = delete;

        // ---- scene (helpers create assets the fixture releases at the end) ----

        scene::entity add_unit_cube(const Eigen::Vector3f& position, const scene::material_description& material = {});
        scene::entity
            add_sphere(const Eigen::Vector3f& position, float radius, const scene::material_description& material = {});
        // A square in the XZ plane, facing +Y, `size` metres wide
        scene::entity add_ground_plane(float size, const scene::material_description& material = {});
        // A directional light shining along `direction`; also the environment's sun
        scene::entity add_sun(const Eigen::Vector3f& direction, float illuminance = 100'000.0f);
        scene::entity add_point_light(const Eigen::Vector3f& position, float intensity, float range);
        // A width × height quad in the XY plane, facing +Z, with an unlit material of `color` (linear)
        scene::entity add_unlit_quad(const Eigen::Vector3f& position, float width, float height, scene::color color);

        // The tick sequence (08 §9.6) into the snapshot buffer: update transforms, make_snapshot, publish, end_tick
        void publish();

        // ---- views and frames ----

        // An RGBA8 output texture that copies can write and read
        rhi::texture_handle make_output(graphics::texture_extent size);

        // A view named `name` of the output's size, at the origin looking down −Z, writing `output` (the fixture's
        // output when none is given)
        visualizer::rendering::view_description
            view(const std::string& name = "main", std::optional<rhi::texture_handle> output = {});

        // Renders `frames` frames of the latest snapshot (publishing an empty scene first if nothing was published),
        // each through begin_frame, retire, render, a GPU wait and end_frame, then reads back every view's output. One
        // image per view, empty for a view without output.
        std::vector<rgba8_image>
            render_and_read(std::span<const visualizer::rendering::view_description> views, int frames = 1);

        // One frame, returning the render's result instead of requiring success
        result<graphics::submitted_frame> render_frame(std::span<const visualizer::rendering::view_description> views);

        rgba8_image read_output(rhi::texture_handle texture);

        // Destroys the renderer (frames must have completed); create_renderer makes a new one
        void destroy_renderer();
        void create_renderer();

        runtime runtime_{ small_runtime_configuration() };
        render_graph_fixture graphics;
        std::unique_ptr<graphics::root_signature_registry> root_signatures;
        std::unique_ptr<graphics::pipeline_cache> pipeline_cache;
        std::unique_ptr<graphics::upload_queue> uploads;
        std::unique_ptr<graphics::readback_queue> readbacks;
        std::optional<graphics::shader_library> shaders;
        visualizer::rendering::render_settings settings;
        std::unique_ptr<visualizer::rendering::scene_renderer> renderer;
        graphics::texture_extent output_size;
        rhi::texture_handle output;

        scene::asset_cache assets;
        scene::scene world{ assets };
        scene::snapshot_buffer snapshots;

    private:
        scene::entity add_drawn(
            const std::string& name,
            const scene::transform& local,
            scene::mesh_data mesh,
            const scene::material_description& material);
        scene::entity add_entity(const std::string& name, const scene::transform& local);

        std::vector<rhi::texture_handle> outputs_;
        std::vector<scene::mesh_handle> meshes_;
        std::vector<scene::material_handle> materials_;
        scene::object_id next_id_ = 1;
    };
} // namespace avis::tests

#endif
