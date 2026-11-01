#ifndef AVIS_VISUALIZER_RENDERING_SCENE_RENDERER_H
#define AVIS_VISUALIZER_RENDERING_SCENE_RENDERER_H

// The scene renderer (docs/design/11-renderer.md): turns one scene::snapshot into an image for each view, on the
// render graph (10). Scene-agnostic: it reads the snapshot and the views, nothing else (11 §2.1).
//
// So far (task 11.2) each view's frame is a placeholder tonemap pass that clears ldr_color to background_color, and
// present_to_shared, which copies it into the view's output. The passes of 11 §2.3 replace the placeholder step by
// step.

#include "rendering/render_settings.h"
#include "rendering/view.h"
#include "rendering/view_history.h"

#include "avis/error.h"
#include "avis/graphics/frame_token.h"
#include "avis/graphics/graph_debug_information.h"
#include "avis/graphics/render_graph.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/resources.h"
#include "avis/scene/snapshot.h"
#include "avis/sender_of.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace avis::graphics
{
    class frame_scheduler;
    class gpu_profiler;
    class pipeline_cache;
    class root_signature_registry;
    class shader_library;
    class transient_resource_pool;
    class upload_queue;
} // namespace avis::graphics

namespace avis::visualizer::rendering
{
    // ldr_color's content until the sky and the tonemap exist (11.11, 11.16): sRGB-encoded, exact in RGBA8
    // (51, 102, 153, 255)
    inline constexpr std::array<float, 4> background_color{ 0.2f, 0.4f, 0.6f, 1.0f };

    // What the renderer works with; everything must outlive it. The caller owns the frame chain: it begins frames on
    // `frames`, calls gpu_profiler::prepare_frame before render, and ends the frames.
    struct renderer_dependencies
    {
        rhi::resources resources;
        rhi::pipelines pipelines;
        graphics::render_graph& graph;
        graphics::transient_resource_pool& pool;
        graphics::frame_scheduler& frames;
        graphics::gpu_profiler* profiler; // may be null: no GPU timings
        graphics::pipeline_cache& pipeline_cache;
        graphics::root_signature_registry& root_signatures; // without a signature named "standard" yet
        graphics::upload_queue& uploads; // per frame: residency uploads, flushed by the renderer's prepare stage
    };

    // For 12's Render Debug panel (11.20 adds the counters of the passes)
    struct renderer_debug_information
    {
        const graphics::graph_debug_information* graph = nullptr; // the last frame's; valid until the next render
        std::uint64_t frames_rendered = 0;
        std::uint64_t last_frame_index = 0;
        std::uint64_t completed_frame_index = 0; // from retire
        std::uint64_t last_publish_sequence = 0; // of the last snapshot rendered
        std::uint32_t views = 0; // in the last frame
        std::size_t view_histories = 0;
    };

    class scene_renderer
    {
    public:
        // Registers the standard root signature (18 §9) as "standard" and creates the renderer's persistent resources.
        // The shader library must outlive the renderer.
        static result<std::unique_ptr<scene_renderer>> create(
            const renderer_dependencies& dependencies,
            const render_settings& settings,
            graphics::shader_library& shaders);

        ~scene_renderer();
        scene_renderer(const scene_renderer&) = delete;
        scene_renderer& operator=(const scene_renderer&) = delete;

        // One frame: builds and compiles the graph on the calling thread (the frame chain's prepare stage), then
        // returns the sender that executes it (10 §7), completing with the frame's submitted_frame for end_frame.
        // The frame, the snapshot's data used by the passes, the renderer and its graph must outlive the sender; the
        // views are copied while building. A frame that can't be built (an invalid view, history resources that can't
        // be created) completes with that error and submits nothing.
        //
        // Views must have distinct, non-empty names without '/' and a non-zero size. An output must be an
        // RGBA8_UNORM texture of the view's size that allows copy_destination.
        auto render(
            graphics::frame_token& frame,
            const scene::snapshot& snapshot,
            std::span<const view_description> views,
            std::chrono::steady_clock::time_point wall_now)
        {
            build_failure_ = build_frame(frame, snapshot, views, wall_now);
            auto sender = stdexec::just() |
                          stdexec::let_value(
                              [this, &frame]()
                              {
                                  if (build_failure_)
                                  {
                                      throw avis::runtime_error{ *build_failure_ };
                                  }
                                  return graph_.execute(frame);
                              }) |
                          threading::into_internal_error();
            static_assert(sender_of<decltype(sender), graphics::submitted_frame>);
            return sender;
        }

        // Frame `completed_frame_index` and every earlier one are complete on the GPU: called by the frame chain when
        // begin_frame recycles a context (11 §3.6, 17 §4)
        void retire(std::uint64_t completed_frame_index) noexcept;

        renderer_debug_information debug_information() const;

        const render_settings& settings() const noexcept
        {
            return settings_;
        }

        // For tests: the view histories (10 §8)
        const view_history_store& view_histories() const noexcept
        {
            return histories_;
        }

    private:
        scene_renderer(
            const renderer_dependencies& dependencies,
            const render_settings& settings,
            graphics::shader_library& shaders,
            rhi::root_signature_handle standard_signature);

        // The graph of one frame, compiled; the first failure otherwise
        std::optional<error> build_frame(
            const graphics::frame_token& frame,
            const scene::snapshot& snapshot,
            std::span<const view_description> views,
            std::chrono::steady_clock::time_point wall_now);

        std::optional<error> check_view(const view_description& view) const;
        std::optional<error> add_view(const graphics::frame_token& frame, const view_description& view);

        renderer_dependencies dependencies_;
        graphics::render_graph& graph_;
        render_settings settings_;
        graphics::shader_library& shaders_;
        rhi::root_signature_handle standard_signature_;
        view_history_store histories_;

        std::optional<error> build_failure_; // of the last render; thrown by its sender
        std::uint64_t frames_rendered_ = 0;
        std::uint64_t last_frame_index_ = 0;
        std::uint64_t completed_frame_index_ = 0;
        std::uint64_t last_publish_sequence_ = 0;
        std::uint32_t last_view_count_ = 0;
    };
} // namespace avis::visualizer::rendering

#endif
