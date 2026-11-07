#ifndef AVIS_VISUALIZER_RENDERING_RENDER_SYSTEM_H
#define AVIS_VISUALIZER_RENDERING_RENDER_SYSTEM_H

// The Visualizer's graphics objects (docs/design/12-visualizer.md §3.2 step 3, §3.4, D-96): the RHI on Qt's adapter
// and everything built on it, created in one place and destroyed in the reverse order. The only holder of the RHI;
// every component gets the views it uses. Free of Qt, so the tests create the same set on WARP.

#include "rendering/render_settings.h"

#include "avis/error.h"
#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/gpu_profiler.h"
#include "avis/graphics/pipeline_cache.h"
#include "avis/graphics/readback_queue.h"
#include "avis/graphics/render_graph.h"
#include "avis/graphics/root_signature_registry.h"
#include "avis/graphics/shader_library.h"
#include "avis/graphics/transient_resource_pool.h"
#include "avis/graphics/upload_queue.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/render_hardware_interface.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/types.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace avis::visualizer::rendering
{
    class scene_renderer;

    struct render_system_configuration
    {
        // Qt's adapter (12 §3.2 step 3), so that both devices are on the GPU that shared handles need. Empty: the
        // RHI's default order.
        std::optional<rhi::adapter_luid> adapter;
        bool allow_software_adapter = false; // WARP, for the tests

        // The `render` section: debug layer, GPU-based validation, frames in flight, the graph's options, and the
        // scene renderer's own settings
        render_settings settings;

        // Where the renderer's shaders are (resolve_shader_directory)
        std::filesystem::path shader_directory;
    };

    // Created on the GUI thread once Qt's adapter is known, and destroyed there. Not copyable or movable: the
    // components refer to each other.
    class render_system
    {
    public:
        // Creates, in this order: the RHI (with its capability gate, 09 §4.4), the frame scheduler with
        // `frames_in_flight`, the GPU profiler, the transient pool and the render graph, the upload and readback
        // queues, the root signature registry and the pipeline cache, the renderer's shaders (load_renderer_shaders,
        // waited for on the calling thread), then the scene renderer. The first failure is returned with the step
        // that failed in front of its message ("the graphics device could not be created: …"), and what was already
        // created is destroyed.
        static result<std::unique_ptr<render_system>> create(const render_system_configuration& configuration);

        // The shutdown of 12 §3.4, after the frame loop has drained and the completion notifier is gone: waits for
        // the GPU (which also completes the senders still waiting on it), destroys the graphics objects in the
        // reverse creation order, then the RHI. Not on a parallel worker or inside a completion.
        ~render_system();

        render_system(const render_system&) = delete;
        render_system& operator=(const render_system&) = delete;

        // The adapter the RHI chose (Qt's, when one was given)
        const rhi::hardware_capabilities& capabilities() const noexcept;

        // For the application's completion notifier (12 §3.4, D-93): poll_completions() and its event
        rhi::completion_driver completion_driver() noexcept;
        rhi::synchronization synchronization() noexcept;

        graphics::frame_scheduler& frames() noexcept;
        graphics::gpu_profiler& profiler() noexcept;
        graphics::render_graph& graph() noexcept;
        graphics::upload_queue& uploads() noexcept;
        graphics::readback_queue& readbacks() noexcept;
        scene_renderer& renderer() noexcept;

        // The root, for the composition's own views (the shared frame ring's sharing view, 12.4) and for tests
        // (D-96). Components take views, never this.
        rhi::render_hardware_interface& hardware_interface() noexcept;

    private:
        render_system() noexcept;

        // Destroyed last, in reverse: the members below use it
        std::unique_ptr<rhi::render_hardware_interface> rhi_;

        std::unique_ptr<graphics::frame_scheduler> frames_;
        std::unique_ptr<graphics::gpu_profiler> profiler_;
        std::unique_ptr<graphics::transient_resource_pool> transient_pool_;
        std::unique_ptr<graphics::render_graph> graph_;
        std::unique_ptr<graphics::upload_queue> uploads_;
        std::unique_ptr<graphics::readback_queue> readbacks_;
        std::unique_ptr<graphics::root_signature_registry> root_signatures_;
        std::unique_ptr<graphics::pipeline_cache> pipeline_cache_;
        std::unique_ptr<graphics::shader_library> shaders_; // outlives the renderer
        std::unique_ptr<scene_renderer> renderer_;
    };
} // namespace avis::visualizer::rendering

#endif
