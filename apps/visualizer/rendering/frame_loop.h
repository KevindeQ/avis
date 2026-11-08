#ifndef AVIS_VISUALIZER_RENDERING_FRAME_LOOP_H
#define AVIS_VISUALIZER_RENDERING_FRAME_LOOP_H

// The Visualizer's frame chain (docs/design/12-visualizer.md §3.4, 17 §4, D-98, D-102): frames rendered from the
// latest snapshot into the shared frame ring, with no thread of its own. Each frame is
//
//   when_all(begin_frame(), next_frame_tick(), shared_slot_free())
//     → continues_on(parallel) → prepare_frame, retire, the snapshot, scene_renderer::render (build, then execute)
//     → end_frame, signal `produced` → re-arm the next frame
//
// and, per frame, a when_produced sender that publishes latest_completed and wakes the viewport. Everything runs in
// one async_scope. GPU waits complete from the RHI's completion driver (the Visualizer's QWinEventNotifier), timers on
// the IO context, CPU work on parallel workers. Free of Qt.
//
// Threads: created, started, stopped and destroyed on one thread (the GUI thread). The setters and request_* may be
// called from any thread; the chain reads them once per frame (12 §4, "GUI → renderer"). The sink is called from the
// thread that ends a frame, a parallel worker.

#include "rendering/debug_blit.h"
#include "rendering/passes/overlay_pass.h"
#include "rendering/picking.h"
#include "rendering/render_settings.h"
#include "rendering/scene_renderer.h"
#include "rendering/screenshot_capture.h"
#include "rendering/view.h"

#include "avis/error.h"
#include "avis/graphics/graph_debug_information.h"
#include "avis/graphics/render_graph.h"
#include "avis/rhi/types.h"
#include "avis/scene/types.h"

#include <asio/steady_timer.hpp>
#include <exec/async_scope.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace avis::io
{
    class io_context;
} // namespace avis::io

namespace avis::scene
{
    class snapshot_buffer;
} // namespace avis::scene

namespace avis::visualizer::rendering
{
    class render_system;
    class shared_frame_ring;
    class shared_frame_rings;

    // The renderer settings the GUI may change at run time (12 §6, Render menu); the next frame applies them
    struct render_options
    {
        ambient_occlusion_settings ao;
        exposure_settings exposure;
        tonemap_operator tonemapper = tonemap_operator::aces;
        bool taa = true; // with render.taa.enabled
        scene::color selection_color = default_selection_color; // linear

        // From the `render` section, with the default outline colour
        static render_options from_settings(const render_settings& settings);

        friend bool operator==(const render_options&, const render_options&) = default;
    };

    struct frame_loop_settings
    {
        float maximum_fps = 0.0f; // render.maximum_fps; 0: the display's refresh rate
        float hidden_fps = 5.0f; // when the window is minimised or occluded (12 §3.4)
        float default_refresh_rate = 60.0f; // until the GUI reports the screen's
        std::chrono::milliseconds debug_interval{ 250 }; // the debug information's period (about 4 Hz)
        // How the main view is named in the graph (its qualified resource names, "main/ldr_color")
        std::string view_name = "main";
    };

    // Counters, for the statistics overlay and tests
    struct frame_loop_statistics
    {
        std::uint64_t ticks = 0; // next_frame_tick completions
        std::uint64_t frames_rendered = 0; // submitted and signalled on `produced`
        std::uint64_t frames_skipped = 0; // ended with no submissions: transient memory exhausted (D-98)
        std::uint64_t frames_failed = 0; // any other error but device loss; the chain goes on
        std::uint64_t frames_published = 0; // when_produced completed and publish() ran
        std::uint64_t wakes = 0; // publish() asked to wake the viewport
        std::uint64_t idle_ticks = 0; // ticks without a ring to render into (no viewport size yet)
    };

    // A pass of the last frame's graph, owned (graphics::pass_debug_information points into the graph's arena)
    struct frame_pass_information
    {
        std::string name;
        graphics::pass_type type = graphics::pass_type::graphics;
        rhi::queue_type queue = rhi::queue_type::graphics;
        bool culled = false;
        std::optional<double> gpu_milliseconds;
    };

    // A resource of the last frame's graph, owned
    struct frame_resource_information
    {
        std::string name;
        bool is_texture = true;
        graphics::texture_description texture;
        graphics::buffer_description buffer;
        bool imported = false;
        bool aliased = false;
        std::uint64_t heap_offset = graphics::resource_debug_information::no_offset;
    };

    // What the Render Debug panel shows (12 §6), as values the GUI can keep
    struct frame_debug_information
    {
        renderer_debug_information renderer; // `graph` is null: passes and resources below are its copy
        std::vector<frame_pass_information> passes;
        std::vector<frame_resource_information> resources;
        std::uint64_t transient_heap_size = 0;
        std::uint64_t aliased_bytes = 0;
        std::uint64_t non_aliased_bytes = 0;
        frame_loop_statistics loop;
    };

    // Renderer → GUI (12 §4): called on a parallel worker, so the implementation forwards with a queued call
    // (ui::renderer_bridge). Only called while the frame loop runs.
    class frame_sink
    {
    public:
        virtual ~frame_sink() = default;

        // About every frame_loop_settings::debug_interval
        virtual void debug_information_changed(frame_debug_information information) = 0;
        // The results that completed since the last call, in request order; not called when there are none
        virtual void picks_completed(std::vector<pick_result> results) = 0;
        virtual void inspections_completed(std::vector<inspector_result> results) = 0;
        virtual void screenshots_completed(std::vector<screenshot_result> results) = 0;
        // The device was lost (12 §7): the chain has stopped re-arming. Called once.
        virtual void fatal_error(avis::error error) = 0;

    protected:
        frame_sink() = default;
        frame_sink(const frame_sink&) = default;
        frame_sink& operator=(const frame_sink&) = default;
    };

    class frame_loop
    {
    public:
        // Everything must outlive the loop. `wake` posts the viewport's update() (viewport_item::make_wake); it may be
        // empty. The rings take their size from the viewport (request_resize); until they have one, the loop only
        // ticks.
        frame_loop(
            render_system& system,
            shared_frame_rings& rings,
            scene::snapshot_buffer& snapshots,
            io::io_context& io,
            frame_sink& sink,
            std::function<void()> wake,
            frame_loop_settings settings,
            const render_options& options);

        // stop()
        ~frame_loop();

        frame_loop(const frame_loop&) = delete;
        frame_loop& operator=(const frame_loop&) = delete;

        // Arms the first frame. Once.
        void start();

        // Requests stop and waits until the scope is empty: the frame being prepared or executed finishes (its token
        // ends, with its submissions when it submitted any), the waits on the GPU, the tick and the ring end stopped,
        // and nothing re-arms. Polls the RHI's completions while it waits, since the caller is the thread that
        // normally drives them. Idempotent.
        void stop();

        // ---- GUI → renderer: latest-value state, any thread ----

        void set_camera(const camera& value, float device_pixel_ratio = 1.0f);
        void set_debug_blits(std::vector<debug_selection> selections);
        void set_render_options(const render_options& options);

        // The window's screen refresh rate (Hz, ≤ 0: unknown) and whether the window can be seen; changes the tick
        // rate from the next tick
        void set_display(float refresh_rate, bool visible);

        // One-shot requests, taken by the next frame that renders the main view
        void request_pick(pick_request request);
        void request_inspection(inspector_request request);
        // Forwarded to scene_renderer::request_screenshot by the next frame
        void request_screenshot(screenshot_request request);

        frame_loop_statistics statistics() const noexcept;

        // The tick period (12 §3.4): `hidden_fps` while the window can't be seen, otherwise `maximum_fps` when it is
        // set, otherwise the refresh rate (the default one when unknown)
        static std::chrono::nanoseconds
            tick_interval(const frame_loop_settings& settings, float refresh_rate, bool visible) noexcept;

    private:
        struct controls
        {
            rendering::camera camera;
            float device_pixel_ratio = 1.0f;
            std::vector<debug_selection> debug_blits;
            render_options options;
            std::uint64_t options_version = 1;
            float refresh_rate = 0.0f;
            bool visible = true;
            std::optional<pick_request> pick;
            std::optional<inspector_request> inspect;
            std::vector<screenshot_request> screenshots;
        };

        // The chain's steps, on the chain's thread at the time (one at a time)
        void arm() noexcept;
        auto next_frame_tick();
        void on_frame_error(const avis::error& error) noexcept;
        void after_frame() noexcept;
        void publish_results();

        // Spawns the publish of `frame` in `ring`'s `slot` once `produced` reaches it
        void spawn_publish(shared_frame_ring& ring, std::uint64_t frame, std::uint32_t slot);

        render_system& system_;
        shared_frame_rings& rings_;
        scene::snapshot_buffer& snapshots_;
        frame_sink& sink_;
        std::function<void()> wake_;
        frame_loop_settings settings_;

        mutable std::mutex controls_mutex_;
        controls controls_;
        std::uint64_t applied_options_version_ = 0; // the chain's

        asio::steady_timer timer_;
        std::chrono::steady_clock::time_point next_tick_{}; // the chain's

        std::chrono::steady_clock::time_point next_debug_{}; // the chain's
        std::chrono::steady_clock::time_point next_failure_log_{}; // the chain's

        // Publishes not yet completed: retired rings are destroyed only while none is, since a publish uses its ring
        std::atomic<std::uint32_t> publishes_in_flight_{ 0 };

        struct counters
        {
            std::atomic<std::uint64_t> ticks{ 0 };
            std::atomic<std::uint64_t> frames_rendered{ 0 };
            std::atomic<std::uint64_t> frames_skipped{ 0 };
            std::atomic<std::uint64_t> frames_failed{ 0 };
            std::atomic<std::uint64_t> frames_published{ 0 };
            std::atomic<std::uint64_t> wakes{ 0 };
            std::atomic<std::uint64_t> idle_ticks{ 0 };
        } counters_;

        std::atomic<bool> started_{ false };
        std::atomic<bool> stopping_{ false };
        std::atomic<bool> fatal_{ false };
        bool stopped_ = false; // stop() completed

        exec::async_scope scope_;
    };
} // namespace avis::visualizer::rendering

#endif
