#include "rendering/frame_loop.h"

#include "rendering/render_system.h"
#include "rendering/shared_frame_ring.h"

#include "avis/graphics/frame_scheduler.h"
#include "avis/graphics/frame_token.h"
#include "avis/graphics/gpu_profiler.h"
#include "avis/io/asio_use_sender.h"
#include "avis/io/io_context.h"
#include "avis/log.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/error.h"
#include "avis/rhi/queue.h"
#include "avis/scene/snapshot.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <span>
#include <thread>
#include <utility>

namespace avis::visualizer::rendering
{
    namespace
    {
        constexpr std::string_view logger_name = "render";

        // A failure is logged at most this often; the counters keep the rest
        constexpr std::chrono::seconds failure_log_interval{ 1 };

        // Receives the scope's on_empty() in stop()
        struct empty_receiver
        {
            using receiver_concept = stdexec::receiver_tag;

            void set_value() noexcept
            {
                empty->store(true, std::memory_order_release);
            }

            void set_stopped() noexcept
            {
                empty->store(true, std::memory_order_release);
            }

            stdexec::env<> get_env() const noexcept
            {
                return {};
            }

            std::atomic<bool>* empty;
        };

        // The empty scene a frame renders before the simulation thread's first publish
        const scene::snapshot& empty_snapshot()
        {
            static const scene::snapshot empty;
            return empty;
        }
    } // namespace

    render_options render_options::from_settings(const render_settings& settings)
    {
        return { .ao = settings.ao,
                 .exposure = settings.exposure,
                 .tonemapper = settings.tonemapper,
                 .taa = settings.taa.enabled };
    }

    frame_loop::frame_loop(
        render_system& system,
        shared_frame_rings& rings,
        scene::snapshot_buffer& snapshots,
        io::io_context& io,
        frame_sink& sink,
        std::function<void()> wake,
        frame_loop_settings settings,
        const render_options& options) :
        system_{ system },
        rings_{ rings },
        snapshots_{ snapshots },
        sink_{ sink },
        wake_{ std::move(wake) },
        settings_{ std::move(settings) },
        timer_{ io.native() }
    {
        controls_.options = options;
    }

    frame_loop::~frame_loop()
    {
        stop();
    }

    void frame_loop::start()
    {
        if (started_.exchange(true))
        {
            return;
        }
        arm();
    }

    void frame_loop::stop()
    {
        if (stopped_)
        {
            return;
        }
        stopping_.store(true);
        scope_.request_stop();

        // The waits on the GPU end stopped through their stop callbacks; a frame in its prepare or execute stage
        // finishes on its worker. Polling covers a wait whose fence completed just before the stop.
        std::atomic<bool> empty{ false };
        auto operation = stdexec::connect(scope_.on_empty(), empty_receiver{ &empty });
        stdexec::start(operation);
        rhi::completion_driver driver = system_.completion_driver();
        while (!empty.load(std::memory_order_acquire))
        {
            driver.poll_completions();
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
        stopped_ = true;

        if (started_.load())
        {
            const frame_loop_statistics counts = statistics();
            log::get(logger_name)
                ->info(
                    "the frame loop stopped: {} frames rendered, {} published, {} skipped, {} failed, {} ticks ({} "
                    "without a viewport size)",
                    counts.frames_rendered,
                    counts.frames_published,
                    counts.frames_skipped,
                    counts.frames_failed,
                    counts.ticks,
                    counts.idle_ticks);
        }
    }

    void frame_loop::set_camera(const camera& value, const float device_pixel_ratio)
    {
        const std::scoped_lock lock{ controls_mutex_ };
        controls_.camera = value;
        controls_.device_pixel_ratio = device_pixel_ratio;
    }

    void frame_loop::set_debug_blits(std::vector<debug_selection> selections)
    {
        const std::scoped_lock lock{ controls_mutex_ };
        controls_.debug_blits = std::move(selections);
    }

    void frame_loop::set_render_options(const render_options& options)
    {
        const std::scoped_lock lock{ controls_mutex_ };
        if (options != controls_.options)
        {
            controls_.options = options;
            ++controls_.options_version;
        }
    }

    void frame_loop::set_display(const float refresh_rate, const bool visible)
    {
        const std::scoped_lock lock{ controls_mutex_ };
        controls_.refresh_rate = refresh_rate;
        controls_.visible = visible;
    }

    void frame_loop::request_pick(const pick_request request)
    {
        const std::scoped_lock lock{ controls_mutex_ };
        controls_.pick = request;
    }

    void frame_loop::request_inspection(const inspector_request request)
    {
        const std::scoped_lock lock{ controls_mutex_ };
        controls_.inspect = request;
    }

    void frame_loop::request_screenshot(screenshot_request request)
    {
        const std::scoped_lock lock{ controls_mutex_ };
        controls_.screenshots.push_back(std::move(request));
    }

    frame_loop_statistics frame_loop::statistics() const noexcept
    {
        return { .ticks = counters_.ticks.load(),
                 .frames_rendered = counters_.frames_rendered.load(),
                 .frames_skipped = counters_.frames_skipped.load(),
                 .frames_failed = counters_.frames_failed.load(),
                 .frames_published = counters_.frames_published.load(),
                 .wakes = counters_.wakes.load(),
                 .idle_ticks = counters_.idle_ticks.load() };
    }

    std::chrono::nanoseconds frame_loop::tick_interval(
        const frame_loop_settings& settings, const float refresh_rate, const bool visible) noexcept
    {
        float rate = settings.default_refresh_rate;
        if (!visible)
        {
            rate = settings.hidden_fps;
        }
        else if (settings.maximum_fps > 0.0f)
        {
            rate = settings.maximum_fps;
        }
        else if (refresh_rate > 0.0f)
        {
            rate = refresh_rate;
        }
        rate = std::clamp(rate, 0.1f, 10'000.0f);
        return std::chrono::nanoseconds{ static_cast<std::int64_t>(std::llround(1.0e9 / static_cast<double>(rate))) };
    }

    auto frame_loop::next_frame_tick()
    {
        float refresh_rate = 0.0f;
        bool visible = true;
        {
            const std::scoped_lock lock{ controls_mutex_ };
            refresh_rate = controls_.refresh_rate;
            visible = controls_.visible;
        }

        // Paced from the previous tick, so a frame that took part of the period doesn't stretch it; a chain that fell
        // behind ticks at once rather than catching up with a burst
        const auto now = std::chrono::steady_clock::now();
        const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            tick_interval(settings_, refresh_rate, visible));
        next_tick_ = std::max(next_tick_ + interval, now);
        timer_.expires_at(next_tick_);
        return timer_.async_wait(io::use_sender) | stdexec::then([this]() noexcept { ++counters_.ticks; });
    }

    void frame_loop::arm() noexcept
    {
        if (stopping_.load() || fatal_.load())
        {
            return;
        }

        try
        {
            // A retired ring may be destroyed once Qt and the GPU are done with it (12 §3.3 rule 8), and no publish
            // still holds it
            if (publishes_in_flight_.load() == 0)
            {
                rings_.destroy_released();
            }

            // A size the viewport reported makes a new ring at the start of this frame
            if (const result<shared_frame_ring*> updated = rings_.update(); !updated)
            {
                log::get(logger_name)
                    ->warn("the viewport's frame ring could not be resized: {}", std::format("{}", updated.error()));
            }

            shared_frame_ring* const ring = rings_.current();
            if (ring == nullptr)
            {
                // No viewport size yet: tick, and look again
                scope_.spawn(
                    next_frame_tick() | stdexec::then([this]() noexcept { ++counters_.idle_ticks; }) |
                    threading::into_internal_error() |
                    stdexec::upon_error([this](avis::error error) noexcept { on_frame_error(error); }) |
                    stdexec::upon_stopped([]() noexcept {}) | stdexec::then([this]() noexcept { after_frame(); }));
                return;
            }

            auto prepare = [this, ring](graphics::frame_token& token, ring_slot& slot)
            {
                // Recycling the context retires what frame N - F used (11 §3.6, 17 §4)
                system_.profiler().prepare_frame(token);
                system_.renderer().retire(token.completed_index());

                // The GUI's state, read once (12 §4); the one-shot requests are taken
                controls taken;
                {
                    const std::scoped_lock lock{ controls_mutex_ };
                    taken.camera = controls_.camera;
                    taken.device_pixel_ratio = controls_.device_pixel_ratio;
                    taken.debug_blits = controls_.debug_blits;
                    taken.options = controls_.options;
                    taken.options_version = controls_.options_version;
                    taken.pick = std::exchange(controls_.pick, std::nullopt);
                    taken.inspect = std::exchange(controls_.inspect, std::nullopt);
                    taken.screenshots = std::exchange(controls_.screenshots, {});
                }

                scene_renderer& renderer = system_.renderer();
                if (taken.options_version != applied_options_version_)
                {
                    renderer.set_ao_parameters(taken.options.ao);
                    renderer.set_exposure_parameters(taken.options.exposure);
                    renderer.set_tonemapper(taken.options.tonemapper);
                    renderer.set_selection_color(taken.options.selection_color);
                    applied_options_version_ = taken.options_version;
                }
                for (screenshot_request& request : taken.screenshots)
                {
                    request.view = settings_.view_name;
                    renderer.request_screenshot(std::move(request));
                }

                std::vector<view_description> views(1);
                view_description& view = views.front();
                view.name = settings_.view_name;
                view.camera = taken.camera;
                view.size = ring->size();
                view.device_pixel_ratio = taken.device_pixel_ratio;
                view.output = ring->output(slot);
                view.options.taa = taken.options.taa;
                view.options.debug_blits = std::move(taken.debug_blits);
                view.options.pick = taken.pick;
                view.options.inspect = taken.inspect;

                // Held until the frame is submitted: the passes read the snapshot's data while they record
                return stdexec::just(snapshots_.acquire(), std::move(views)) |
                       stdexec::let_value(
                           [this, &token](scene::snapshot_reference& snapshot, std::vector<view_description>& views)
                           {
                               const scene::snapshot& source = snapshot ? *snapshot : empty_snapshot();
                               return system_.renderer().render(
                                   token,
                                   source,
                                   std::span<const view_description>{ views },
                                   std::chrono::steady_clock::now());
                           }) |
                       stdexec::then(
                           [this, ring, &token, &slot](const graphics::submitted_frame& submitted)
                           {
                               const std::uint64_t index = token.index();
                               const std::uint32_t slot_index = slot.index();
                               system_.frames().end_frame(std::move(token), submitted.last_submissions());

                               // After the frame's submissions, on the queue present_to_shared ran on (12 §3.3)
                               rhi::queue<rhi::queue_type::graphics> queue{ system_.hardware_interface() };
                               ring->signal_produced(queue, std::move(slot), index);
                               ++counters_.frames_rendered;
                               spawn_publish(*ring, index, slot_index);
                               publish_results();
                           });
            };

            scope_.spawn(
                stdexec::when_all(system_.frames().begin_frame(), next_frame_tick(), ring->shared_slot_free()) |
                stdexec::continues_on(stdexec::get_parallel_scheduler()) | stdexec::let_value(std::move(prepare)) |
                threading::into_internal_error() |
                stdexec::upon_error([this](avis::error error) noexcept { on_frame_error(error); }) |
                stdexec::upon_stopped([]() noexcept {}) | stdexec::then([this]() noexcept { after_frame(); }));
        }
        catch (...)
        {
            // Nothing re-arms: a loop that can't spawn its next frame can't go on
            const avis::error error = make_error(std::current_exception());
            log::get(logger_name)->error("the frame loop stopped: {}", std::format("{}", error));
            fatal_.store(true);
        }
    }

    void frame_loop::spawn_publish(shared_frame_ring& ring, const std::uint64_t frame, const std::uint32_t slot)
    {
        ++publishes_in_flight_;
        scope_.spawn(
            ring.when_produced(frame) |
            stdexec::then(
                [this, &ring, frame, slot]() noexcept
                {
                    ++counters_.frames_published;
                    if (ring.publish(frame, slot))
                    {
                        ++counters_.wakes;
                        if (wake_)
                        {
                            try
                            {
                                wake_();
                            }
                            catch (...)
                            {
                                // The viewport misses this wake; the next frame's publish wakes it
                            }
                        }
                    }
                }) |
            stdexec::upon_error([](avis::error) noexcept {}) | stdexec::upon_stopped([]() noexcept {}) |
            stdexec::then([this]() noexcept { --publishes_in_flight_; }));
    }

    void frame_loop::publish_results()
    {
        scene_renderer& renderer = system_.renderer();
        if (std::vector<pick_result> picks = renderer.take_pick_results(); !picks.empty())
        {
            sink_.picks_completed(std::move(picks));
        }
        if (std::vector<inspector_result> inspections = renderer.take_inspector_results(); !inspections.empty())
        {
            sink_.inspections_completed(std::move(inspections));
        }
        if (std::vector<screenshot_result> screenshots = renderer.take_screenshot_results(); !screenshots.empty())
        {
            sink_.screenshots_completed(std::move(screenshots));
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < next_debug_)
        {
            return;
        }
        next_debug_ = now + settings_.debug_interval;

        frame_debug_information information;
        information.renderer = renderer.debug_information();
        if (const graphics::graph_debug_information* const graph = information.renderer.graph; graph != nullptr)
        {
            information.passes.reserve(graph->passes.size());
            for (const graphics::pass_debug_information& pass : graph->passes)
            {
                information.passes.push_back(
                    { .name = std::string{ pass.name },
                      .type = pass.type,
                      .queue = pass.queue,
                      .culled = pass.culled,
                      .gpu_milliseconds = pass.gpu_milliseconds });
            }
            information.resources.reserve(graph->resources.size());
            for (const graphics::resource_debug_information& resource : graph->resources)
            {
                information.resources.push_back(
                    { .name = std::string{ resource.name },
                      .is_texture = resource.is_texture,
                      .texture = resource.texture,
                      .buffer = resource.buffer,
                      .imported = resource.imported,
                      .aliased = resource.aliased,
                      .heap_offset = resource.heap_offset });
            }
            information.transient_heap_size = graph->transient_heap_size;
            information.aliased_bytes = graph->aliased_bytes;
            information.non_aliased_bytes = graph->non_aliased_bytes;
        }
        information.renderer.graph = nullptr; // points into the graph's arena, which the next frame resets
        information.loop = statistics();
        sink_.debug_information_changed(std::move(information));
    }

    void frame_loop::on_frame_error(const avis::error& error) noexcept
    {
        try
        {
            if (rhi::is_error(error, rhi::error_kind::device_lost))
            {
                if (!fatal_.exchange(true))
                {
                    log::get(logger_name)->critical("the graphics device was lost: {}", std::format("{}", error));
                    sink_.fatal_error(error);
                }
                return;
            }

            // The frame ended with no submissions: its token and slot ended themselves (D-102). The next frame goes on.
            const bool exhausted = rhi::is_error(error, rhi::error_kind::transient_memory_exhausted);
            ++(exhausted ? counters_.frames_skipped : counters_.frames_failed);
            const auto now = std::chrono::steady_clock::now();
            if (now < next_failure_log_)
            {
                return;
            }
            next_failure_log_ = now + failure_log_interval;
            if (exhausted)
            {
                // D-98: the scheduler's own warning names the allocator and its high-water mark
                log::get(logger_name)
                    ->warn(
                        "a frame was skipped: its per-frame upload memory or descriptors ran out; raise "
                        "upload_memory_per_frame or descriptors_per_frame ({})",
                        error.message);
            }
            else
            {
                log::get(logger_name)->error("a frame failed: {}", std::format("{}", error));
            }
        }
        catch (...)
        {
            // Logging failed; the counters still have it
        }
    }

    void frame_loop::after_frame() noexcept
    {
        arm();
    }
} // namespace avis::visualizer::rendering
