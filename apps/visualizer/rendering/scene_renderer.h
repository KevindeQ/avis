#ifndef AVIS_VISUALIZER_RENDERING_SCENE_RENDERER_H
#define AVIS_VISUALIZER_RENDERING_SCENE_RENDERER_H

// The scene renderer (docs/design/11-renderer.md): turns one scene::snapshot into an image for each view, on the
// render graph (10). Scene-agnostic: it reads the snapshot and the views, nothing else (11 §2.1).
//
// So far (task 11.2) each view's frame is a placeholder tonemap pass that clears ldr_color to background_color, and
// present_to_shared, which copies it into the view's output. The passes of 11 §2.3 replace the placeholder step by
// step. Residency runs in every frame's prepare stage: the snapshot's meshes are uploaded into the geometry arena
// (11.3), its textures into their own RHI textures and its materials into the material table (11.4). The arena and
// the material table are imported into the graph, and the frame generates the mip chains of newly uploaded images.
// Extraction (11.5) then builds the frame's instance and light arrays, interpolated at the frame's time, and pass 0
// (upload_frame_data) copies them into persistent ring-indexed buffers. Startup creates the pipeline sets the passes
// draw with (11.7).

#include "rendering/draw_lists.h"
#include "rendering/extraction.h"
#include "rendering/frame_data.h"
#include "rendering/geometry_arena.h"
#include "rendering/material_table.h"
#include "rendering/pipeline_variant_registry.h"
#include "rendering/render_settings.h"
#include "rendering/residency_cache.h"
#include "rendering/texture_residency.h"
#include "rendering/view.h"
#include "rendering/view_history.h"

#include "avis/error.h"
#include "avis/graphics/buffer.h"
#include "avis/graphics/frame_token.h"
#include "avis/graphics/graph_debug_information.h"
#include "avis/graphics/render_graph.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
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
        rhi::synchronization synchronization; // residency: whether a version's copies are complete (11 §3.6)
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
        residency_statistics residency; // meshes, pending uploads and arena use (11.3)
        texture_residency_statistics textures; // 11.4
        material_table_statistics materials; // 11.4

        // Extraction (11.5), of the last frame: reused is true when it read the previous frame's arrays (11 §3.5)
        struct extraction_counts
        {
            float alpha = 1.0f;
            bool reused = false;
            std::size_t instances = 0;
            std::size_t meshes = 0;
            std::size_t lights = 0;
            std::size_t skipped_invisible = 0;
            std::size_t skipped_not_resident = 0;
            std::size_t skipped_invalid = 0;
            std::size_t dropped_lights = 0;
        } extraction;
        frame_data_statistics frame_data; // pass 0 (11.5)
        pipeline_variant_statistics pipelines; // 11.7
        std::vector<draw_list_statistics> draw_lists; // the last frame's, one per view in view order (11.8)
    };

    // The geometry arena in this frame's graph (11 §3.2), imported outside any view with the newest ready_after of the
    // mesh versions the frame draws (D-103). The passes that draw read them (11.10).
    struct frame_geometry
    {
        graphics::buffer vertices;
        graphics::buffer indices;
    };

    // The material table in this frame's graph (11 §3.3), imported outside any view with the newest ready_after of the
    // slots the frame draws and of their textures. The textures themselves are read bindlessly, in `common`.
    struct frame_materials
    {
        graphics::buffer table;
    };

    class scene_renderer
    {
    public:
        // Registers the standard root signature (18 §9) as "standard", creates the base pipeline sets in a bulk on the
        // parallel scheduler (11.7) and the renderer's persistent resources, the geometry arena among them
        // (geometry.arena_mib; invalid_argument for an arena the RHI can't address). The shader library must outlive
        // the renderer.
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
        // be created, a failed upload flush) completes with that error and submits nothing.
        //
        // Before building the graph, the prepare stage makes the snapshot's meshes resident (11 §3.6): it queues their
        // uploads, flushes the upload queue and imports the geometry arena. It then extracts the snapshot at the time
        // of `wall_now` (11 §3.5) and adds pass 0, which uploads the frame's arrays. When the snapshot's
        // publish_sequence and alpha equal the previous frame's, the previous arrays had no motion and residency has
        // nothing pending, residency and extraction are skipped and the previous arrays are read again.
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
            if (build_failure_)
            {
                frame_data_->forget(); // nothing was copied: a later frame mustn't reuse this frame's slots
            }
            // A frame that fails to execute didn't copy its arrays either
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
                          stdexec::let_error(
                              [this]<typename Error>(Error&& error) noexcept
                              {
                                  frame_data_->forget();
                                  return stdexec::just_error(std::forward<Error>(error));
                              }) |
                          stdexec::let_stopped(
                              [this]() noexcept
                              {
                                  frame_data_->forget();
                                  return stdexec::just_stopped();
                              }) |
                          threading::into_internal_error();
            static_assert(sender_of<decltype(sender), graphics::submitted_frame>);
            return sender;
        }

        // Frame `completed_frame_index` and every earlier one are complete on the GPU: called by the frame chain when
        // begin_frame recycles a context (11 §3.6, 17 §4). Frees the geometry ranges no frame can read any more.
        void retire(std::uint64_t completed_frame_index);

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

        // For tests: mesh residency and the arena (11 §3.6)
        const residency_cache& residency() const noexcept
        {
            return *residency_;
        }

        const geometry_arena& arena() const noexcept
        {
            return *arena_;
        }

        // For tests: texture residency and the material table (11.4)
        const texture_residency& textures() const noexcept
        {
            return *textures_;
        }

        const material_table& materials() const noexcept
        {
            return *materials_;
        }

        // For tests: the last extraction (11.5); a frame that reused it keeps it
        const extraction_result& extraction() const noexcept
        {
            return extraction_;
        }

        const extraction_state& previous_transforms() const noexcept
        {
            return extraction_state_;
        }

        // The pipeline sets (11.7): the draw lists key draws by set, the passes take their PSOs from it
        pipeline_variant_registry& pipeline_variants() noexcept
        {
            return *pipeline_variants_;
        }

        const pipeline_variant_registry& pipeline_variants() const noexcept
        {
            return *pipeline_variants_;
        }

        // The frame data in the graph of the frame being built, for its passes (11.10 on)
        const frame_buffers& frame_data() const noexcept
        {
            return frame_buffers_;
        }

        // The draw lists of the frame being built, one per view in view order (11.8). They live in the graph's arena:
        // valid until the next render.
        std::span<const draw_list> draw_lists() const noexcept
        {
            return draw_lists_;
        }

    private:
        // What create makes before the renderer exists
        struct persistent_state
        {
            rhi::root_signature_handle standard_signature;
            rhi::compute_pipeline_handle generate_mips;
            std::unique_ptr<pipeline_variant_registry> pipeline_variants;
            std::unique_ptr<geometry_arena> arena;
            std::unique_ptr<texture_residency> textures;
            std::unique_ptr<material_table> materials;
        };

        scene_renderer(
            const renderer_dependencies& dependencies,
            const render_settings& settings,
            graphics::shader_library& shaders,
            persistent_state state);

        // The passes that generate the mip chains of this frame's texture_residency::mip_requests(), outside any view
        void add_mip_passes();

        // The reuse shortcut's condition (11 §3.5)
        bool can_reuse_extraction(const scene::snapshot& snapshot, const frame_interpolation& interpolation) const;

        // The graph of one frame, compiled; the first failure otherwise
        std::optional<error> build_frame(
            const graphics::frame_token& frame,
            const scene::snapshot& snapshot,
            std::span<const view_description> views,
            std::chrono::steady_clock::time_point wall_now);

        std::optional<error> check_view(const view_description& view) const;
        std::optional<error>
            add_view(const graphics::frame_token& frame, const scene::snapshot& snapshot, const view_description& view);

        // The view's draw list (11 §4.5), in the graph's arena
        draw_list build_view_draw_list(const scene::snapshot& snapshot, const view_description& view);

        renderer_dependencies dependencies_;
        graphics::render_graph& graph_;
        render_settings settings_;
        graphics::shader_library& shaders_;
        rhi::root_signature_handle standard_signature_;
        view_history_store histories_;
        rhi::compute_pipeline_handle generate_mips_pipeline_;
        std::unique_ptr<pipeline_variant_registry> pipeline_variants_;
        std::unique_ptr<geometry_arena> arena_;
        std::unique_ptr<residency_cache> residency_; // refers to arena_ and the upload queue
        std::unique_ptr<texture_residency> textures_;
        std::unique_ptr<material_table> materials_; // refers to textures_
        frame_geometry geometry_; // of the frame being built
        frame_materials frame_materials_; // of the frame being built
        extraction_state extraction_state_; // the previous rendered worlds
        extraction_result extraction_; // of the last extraction
        bool extraction_reused_ = false; // by the last frame
        std::uint64_t texture_changes_resolved_ = 0; // texture_residency::status_changes() at the last material update
        std::unique_ptr<frame_data_upload> frame_data_;
        frame_buffers frame_buffers_; // of the frame being built
        std::vector<draw_mesh> draw_meshes_; // the frame's meshes for the draw lists, parallel to extraction_.meshes
        std::vector<draw_list> draw_lists_; // of the frame being built, per view
        std::optional<std::chrono::steady_clock::time_point> last_pipeline_warning_;

        std::optional<error> build_failure_; // of the last render; thrown by its sender
        std::uint64_t frames_rendered_ = 0;
        std::uint64_t last_frame_index_ = 0;
        std::uint64_t completed_frame_index_ = 0;
        std::uint64_t last_publish_sequence_ = 0;
        std::uint32_t last_view_count_ = 0;
    };
} // namespace avis::visualizer::rendering

#endif
