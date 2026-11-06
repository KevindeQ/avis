#ifndef AVIS_VISUALIZER_RENDERING_SCENE_RENDERER_H
#define AVIS_VISUALIZER_RENDERING_SCENE_RENDERER_H

// The scene renderer (docs/design/11-renderer.md): turns one scene::snapshot into an image for each view, on the
// render graph (10). Scene-agnostic: it reads the snapshot and the views, nothing else (11 §2.1).
//
// Each view's frame (11 §2.3) is, so far: depth_prepass, linearize_depth with the depth_mips chain, cluster_build and
// light_cull on async compute, gbuffer (11.10), shadow_cascades when the sun casts shadows (11.12), deferred_lighting
// and sky into hdr_color (11.11), forward_transparent (11.14), taa_resolve (11.15), exposure and the tonemap into
// ldr_color (11.16), the selection outline and the overlays (11.17), picking on a click frame (11.19), the debug blits
// and the pixel inspector (11.20), and present_to_shared, which copies ldr_color into the view's output. The cascades
// are computed per view before the draw lists, which cull the shadow casters into them. Before the views, the
// environment baker bakes the sky cubemap, the prefiltered cubemap and the SH9 irradiance when the snapshot's
// environment changes, and the BRDF LUT once (11 §6.4). Residency runs in every frame's prepare stage: the snapshot's
// meshes are uploaded into the geometry arena (11.3), its textures into their own RHI textures and its materials into
// the material table (11.4). The arena and the material table are imported into the graph, and the frame generates the
// mip chains of newly uploaded images. Extraction (11.5) then builds the frame's instance and light arrays,
// interpolated at the frame's time, and pass 0 (upload_frame_data) copies them into persistent ring-indexed buffers.
// Startup creates the pipeline sets the passes draw with (11.7). Each view's draw list (11.8) is cut into runs before
// pass 0, which uploads their instance indices; the passes record their key ranges with the range walker (11.9).

#include "rendering/cascades.h"
#include "rendering/debug_blit.h"
#include "rendering/draw_lists.h"
#include "rendering/environment_baker.h"
#include "rendering/extraction.h"
#include "rendering/frame_data.h"
#include "rendering/geometry_arena.h"
#include "rendering/material_table.h"
#include "rendering/pass_statistics.h"
#include "rendering/passes/overlay_pass.h"
#include "rendering/picking.h"
#include "rendering/pipeline_variant_registry.h"
#include "rendering/progressive_lod.h"
#include "rendering/range_walker.h"
#include "rendering/readback_ring.h"
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
#include "avis/graphics/upload_ticket.h"
#include "avis/rhi/pipeline.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/synchronization.h"
#include "avis/scene/snapshot.h"
#include "avis/sender_of.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

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

    // The items of one group of a view's draw list (11 §4.5)
    struct draw_group_items
    {
        std::string group; // group_name: "scene/camera/world/opaque"
        std::size_t items = 0;
    };

    // A view's light clusters (11 §6.3), read back from light_cull's output
    struct view_cluster_statistics
    {
        std::string view;
        std::uint64_t frame_index = 0; // of the frame it was read back from
        std::uint32_t dropped_lights = 0; // light references dropped past cluster_maximum_lights (overflow)
        std::uint32_t overflowing_clusters = 0; // clusters that dropped any
        std::uint32_t most_lights = 0; // the most lights a cluster touched, dropped ones included
        std::uint32_t light_references = 0; // kept in the light index list
    };

    // For 12's Render Debug panel (11.20): the graph's information and the renderer's counters
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
        std::vector<std::size_t> draw_runs; // the runs of each of those lists (11.9)
        std::size_t instance_indices = 0; // of the last frame's runs, uploaded by pass 0
        // The progressive LOD of each of those lists (11.18), and the vertices its runs draw over every viewport
        std::vector<progressive_lod_statistics> progressive;
        std::vector<std::uint64_t> progressive_vertices;
        environment_statistics environment; // the environment bakes (11.11)

        // The scene passes of the last frame, in declaration order: their parts' draws, instances, pipeline changes
        // and the rest of the range walker's counters (11.9). Complete once that frame's sender has completed.
        std::vector<pass_draw_statistics> passes;
        // The items of each group of the last frame's draw lists, one list per view in view order
        std::vector<std::vector<draw_group_items>> draw_groups;
        // The latest cluster statistics read back for each view of the last frame that writes something; a view
        // whose frames haven't completed yet has none
        std::vector<view_cluster_statistics> clusters;
        // The sum of the GPU times graph.passes carry (frame N - F, from the profiler); none without timings
        std::optional<double> gpu_milliseconds;
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
            const std::uint64_t frame_index = frame.index();
            if (build_failure_)
            {
                forget_frame(); // nothing was copied or baked: a later frame mustn't rely on this one
                drop_readbacks(frame_index);
            }
            // A frame that fails to execute didn't copy its arrays either, nor read anything back. A submitted frame's
            // readbacks wait for its submissions to complete (take_pick_results, take_inspector_results).
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
                          stdexec::then(
                              [this, frame_index](graphics::submitted_frame submitted)
                              {
                                  readbacks_submitted(frame_index, submitted);
                                  return submitted;
                              }) |
                          stdexec::let_error(
                              [this, frame_index]<typename Error>(Error&& error) noexcept
                              {
                                  forget_frame();
                                  drop_readbacks(frame_index);
                                  return stdexec::just_error(std::forward<Error>(error));
                              }) |
                          stdexec::let_stopped(
                              [this, frame_index]() noexcept
                              {
                                  forget_frame();
                                  drop_readbacks(frame_index);
                                  return stdexec::just_stopped();
                              }) |
                          threading::into_internal_error();
            static_assert(sender_of<decltype(sender), graphics::submitted_frame>);
            return sender;
        }

        // Frame `completed_frame_index` and every earlier one are complete on the GPU: called by the frame chain when
        // begin_frame recycles a context (11 §3.6, 17 §4). Frees the geometry ranges no frame can read any more.
        void retire(std::uint64_t completed_frame_index);

        // The results of the pick requests (view_options::pick, 11 §5.1) whose frames are complete on the GPU, in
        // request order, each once. Polled by the frame chain, which posts them to the GUI with its per-frame state. A
        // request whose frame failed, or made while every readback slot was in use, gets no result (the latter logs a
        // warning).
        std::vector<pick_result> take_pick_results();

        // The results of the inspector requests (view_options::inspect, 11 §11) whose frames are complete on the GPU,
        // in request order, each once, like take_pick_results. A request whose blit can't be shown, or whose cursor is
        // outside the blit, gets a result without a value at once; one made while every readback slot was in use
        // gets none.
        std::vector<inspector_result> take_inspector_results();

        // Also collects the cluster statistics whose frames are complete
        renderer_debug_information debug_information() const;

        const render_settings& settings() const noexcept
        {
            return settings_;
        }

        // The GTAO parameters (11 §8), render.ao at creation; for the debug UI (12 §6). Takes effect with the next
        // frame. An intensity of 0 turns GTAO off: its passes aren't added.
        void set_ao_parameters(const ambient_occlusion_settings& parameters) noexcept
        {
            ao_parameters_ = parameters;
        }

        const ambient_occlusion_settings& ao_parameters() const noexcept
        {
            return ao_parameters_;
        }

        // The exposure (11 §10.3), render.exposure at creation; for the debug UI's manual EV100 (12 §6). Takes effect
        // with the next frame, whose tonemap already shows a manual exposure's change.
        void set_exposure_parameters(const exposure_settings& parameters) noexcept
        {
            exposure_parameters_ = parameters;
        }

        const exposure_settings& exposure_parameters() const noexcept
        {
            return exposure_parameters_;
        }

        // The tonemap curve (11 §10.4), render.tonemapper at creation; takes effect with the next frame
        void set_tonemapper(const tonemap_operator tonemapper) noexcept
        {
            tonemapper_ = tonemapper;
        }

        tonemap_operator tonemapper() const noexcept
        {
            return tonemapper_;
        }

        // The selection outline's colour (11 §10.5), linear; its alpha is ignored. Takes effect with the next frame.
        void set_selection_color(const scene::color color) noexcept
        {
            selection_color_ = color;
        }

        scene::color selection_color() const noexcept
        {
            return selection_color_;
        }

        // For tests and tools: replaces the snapshot's environment with a uniform radiance (linear RGB, cd/m²) in every
        // direction, for the image-based lighting and the sky; none restores the sky. Takes effect with the next frame.
        void set_environment_override(std::optional<Eigen::Vector3f> uniform_radiance)
        {
            environment_override_ = uniform_radiance;
        }

        // For tests: the baked environment (11 §6.4)
        const environment_baker& environment() const noexcept
        {
            return *environment_;
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

        // Their runs, one per view in view order, for the passes' range walks (11.9); valid until the next render
        std::span<const prepared_draw_list> prepared_draw_lists() const noexcept
        {
            return prepared_draw_lists_;
        }

        // The frame's instance indices: each draw run's at its instance_offset. Pass 0 uploads them into
        // frame_data().instance_indices.
        std::span<const std::uint32_t> instance_indices() const noexcept
        {
            return instance_indices_;
        }

        // The shadow cascades of the frame being built, one per view in view order (11.12); count 0 for a view without
        // them (no sun, or a sun that casts no shadows). Valid until the next render.
        std::span<const shadow_cascades> view_cascades() const noexcept
        {
            return view_cascades_;
        }

        // The frame's mesh table, parallel to extraction().meshes: what the draw lists and the range walks read
        std::span<const draw_mesh> draw_meshes() const noexcept
        {
            return draw_meshes_;
        }

    private:
        // What create makes before the renderer exists
        struct persistent_state
        {
            rhi::root_signature_handle standard_signature;
            rhi::compute_pipeline_handle generate_mips;
            rhi::compute_pipeline_handle linearize_depth;
            rhi::compute_pipeline_handle depth_mips;
            rhi::compute_pipeline_handle cluster_build;
            rhi::compute_pipeline_handle light_cull;
            rhi::compute_pipeline_handle gtao;
            rhi::compute_pipeline_handle gtao_denoise;
            rhi::compute_pipeline_handle deferred_lighting;
            rhi::graphics_pipeline_handle sky;
            rhi::compute_pipeline_handle taa;
            rhi::compute_pipeline_handle exposure_histogram;
            rhi::compute_pipeline_handle exposure_adapt;
            rhi::compute_pipeline_handle tonemap;
            rhi::compute_pipeline_handle selection_outline;
            rhi::compute_pipeline_handle pick_window;
            rhi::graphics_pipeline_handle debug_blit;
            rhi::compute_pipeline_handle debug_inspect;
            rhi::compute_pipeline_handle cluster_statistics;
            rhi::texture_handle blue_noise; // the tonemap's dither (blue_noise.h)
            graphics::upload_ticket blue_noise_upload;
            std::unique_ptr<environment_baker> environment;
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

        // The frame being built didn't run: its frame data slots, its environment bake, its cluster bounds and its
        // exposure resets are done again by the next frame
        void forget_frame() noexcept;

        // The readback ring of picks (11 §5.1) and inspector texels (11 §11): maximum_readback_slots buffers of
        // readback_slot_bytes. A slot holds one request until its result is taken.
        static constexpr std::size_t maximum_readback_slots = 8;
        static constexpr std::uint64_t readback_slot_bytes =
            std::max<std::uint64_t>(pick_window_bytes, gpu::debug_inspector_bytes);

        struct pick_payload
        {
            std::string view;
            pick_request request;
        };

        struct inspection_payload
        {
            std::string view;
            inspector_request request;
            std::string resource; // qualified
            std::uint32_t mip = 0;
            std::uint32_t slice = 0;
            texel_position texel;
        };

        using readback_payload = std::variant<pick_payload, inspection_payload>;

        // The cluster statistics' ring (debug_information): a slot per view and frame in flight
        static constexpr std::size_t maximum_cluster_statistics_slots = 16;

        // The frame's readbacks were submitted, or its frame failed and its requests get no result
        void readbacks_submitted(std::uint64_t frame_index, const graphics::submitted_frame& submitted);
        void drop_readbacks(std::uint64_t frame_index) noexcept;

        // A slot of the shared ring for a request of frame `frame_index`, or nullptr after a rate-limited warning that
        // every slot is in use; fails when a slot's buffer can't be created
        result<readback_ring<readback_payload>::slot*>
            acquire_readback(const view_description& view, std::uint64_t frame_index, readback_payload payload);

        // The view's debug blits and inspector after the overlays and picking (11 §11); returns the ldr_color they
        // wrote. `resources` are the view's textures a selection may name.
        struct debug_resource;
        result<graphics::texture> add_view_debug_blits(
            const graphics::frame_token& frame,
            const view_description& view,
            std::span<const debug_resource> resources,
            graphics::buffer cluster_lights,
            graphics::texture ldr_color,
            graphics::texture linear_depth,
            const gpu::view_constants& view_constants);

        // An inspector result without a value, at once (none for a frame that fails)
        void inspect_without_value(std::uint64_t frame_index, const view_description& view, std::string resource);

        // Logs a debug selection that can't be shown, at most once per second
        void warn_debug_selection(const view_description& view, const debug_selection& selection, std::string_view why);

        // Moves the cluster statistics whose frames are complete into cluster_statistics_
        void collect_cluster_statistics() const;

        // The reuse shortcut's condition (11 §3.5)
        bool can_reuse_extraction(const scene::snapshot& snapshot, const frame_interpolation& interpolation) const;

        // The graph of one frame, compiled; the first failure otherwise
        std::optional<error> build_frame(
            const graphics::frame_token& frame,
            const scene::snapshot& snapshot,
            std::span<const view_description> views,
            std::chrono::steady_clock::time_point wall_now);

        std::optional<error> check_view(const view_description& view) const;
        // `index` is the view's position in the frame: its draw list. `wall_now` times the exposure's adaptation.
        std::optional<error> add_view(
            const graphics::frame_token& frame,
            const view_description& view,
            std::size_t index,
            std::chrono::steady_clock::time_point wall_now);
        std::optional<error> add_buffer_captures(
            const view_description& view,
            graphics::buffer cluster_lights,
            graphics::buffer light_indices,
            graphics::buffer exposure);

        // The view's shadow cascades (11 §7): none without a sun that casts shadows
        shadow_cascades compute_view_cascades(const view_description& view) const;

        // The view's draw list (11 §4.5), in the graph's arena, with the cascades' shadow casters
        draw_list build_view_draw_list(
            const scene::snapshot& snapshot, const view_description& view, const shadow_cascades& cascades);

        renderer_dependencies dependencies_;
        graphics::render_graph& graph_;
        render_settings settings_;
        ambient_occlusion_settings ao_parameters_; // settings_.ao until set_ao_parameters
        exposure_settings exposure_parameters_; // settings_.exposure until set_exposure_parameters
        tonemap_operator tonemapper_; // settings_.tonemapper until set_tonemapper
        scene::color selection_color_ = default_selection_color;
        graphics::shader_library& shaders_;
        rhi::root_signature_handle standard_signature_;
        view_history_store histories_;
        rhi::compute_pipeline_handle generate_mips_pipeline_;
        rhi::compute_pipeline_handle linearize_depth_pipeline_;
        rhi::compute_pipeline_handle depth_mips_pipeline_;
        rhi::compute_pipeline_handle cluster_build_pipeline_;
        rhi::compute_pipeline_handle light_cull_pipeline_;
        rhi::compute_pipeline_handle gtao_pipeline_;
        rhi::compute_pipeline_handle gtao_denoise_pipeline_;
        rhi::compute_pipeline_handle deferred_lighting_pipeline_;
        rhi::graphics_pipeline_handle sky_pipeline_;
        rhi::compute_pipeline_handle taa_pipeline_;
        rhi::compute_pipeline_handle exposure_histogram_pipeline_;
        rhi::compute_pipeline_handle exposure_adapt_pipeline_;
        rhi::compute_pipeline_handle tonemap_pipeline_;
        rhi::compute_pipeline_handle selection_outline_pipeline_;
        rhi::compute_pipeline_handle pick_window_pipeline_;
        rhi::graphics_pipeline_handle debug_blit_pipeline_;
        rhi::compute_pipeline_handle debug_inspect_pipeline_;
        rhi::compute_pipeline_handle cluster_statistics_pipeline_;
        readback_ring<readback_payload> readbacks_;
        // Inspector results without a value, with their place in readbacks_' order and their frame's index
        struct ready_inspection
        {
            std::uint64_t sequence = 0;
            inspector_result result;
        };
        std::vector<ready_inspection> ready_inspections_;
        std::optional<std::chrono::steady_clock::time_point> last_readback_warning_;
        std::optional<std::chrono::steady_clock::time_point> last_debug_warning_;
        // Collected by debug_information, which is const for its callers
        mutable readback_ring<std::string> cluster_readbacks_; // payload: the view's name
        mutable std::map<std::string, view_cluster_statistics, std::less<>> cluster_statistics_;
        std::vector<std::string> last_view_names_; // of the last frame built
        pass_statistics_collector pass_statistics_; // of the last frame built
        rhi::texture_handle blue_noise_texture_;
        // The blue noise's upload, queued at creation and flushed by the first frame, which resolves it into
        // blue_noise_ready_; every frame's import waits on that point
        std::optional<graphics::upload_ticket> blue_noise_upload_;
        rhi::synchronization_point blue_noise_ready_{};
        graphics::texture blue_noise_; // imported into the frame being built
        std::unique_ptr<environment_baker> environment_;
        std::optional<Eigen::Vector3f> environment_override_;
        float ambient_intensity_ = 1.0f; // the snapshot's, for the frame being built
        environment_resources environment_resources_; // of the frame being built
        std::vector<std::string> rebuilt_clusters_; // the views whose cluster bounds the frame being built rebuilds
        std::vector<std::string> resolved_views_; // the views whose TAA history the frame being built writes
        std::vector<std::string> reset_exposures_; // the views whose exposure buffer the frame being built resets
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
        std::vector<shadow_cascades> view_cascades_; // of the frame being built, per view
        std::vector<draw_list> draw_lists_; // of the frame being built, per view
        std::vector<prepared_draw_list> prepared_draw_lists_; // their runs, per view (11.9)
        std::vector<progressive_lod_statistics> progressive_statistics_; // their LOD, per view (11.18)
        // Each view's LOD state, by view name; a view not rendered in a frame loses it
        std::map<std::string, progressive_lod, std::less<>> progressive_lods_;
        std::vector<std::uint32_t> instance_indices_; // of every view's draw runs, uploaded by pass 0
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
