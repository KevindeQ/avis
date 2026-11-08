// The Visualizer's frame chain (docs/tasks/12-visualizer.md §12.7): the render system on WARP, the shared frame rings,
// the demo scene on the real simulation thread, the GPU's completions driven by a gpu_completion_thread (the
// Visualizer's QWinEventNotifier), and a fake Qt side on a test thread that attaches to the ring, picks the newest
// frame when woken and signals `consumed` from the CPU.

#include "helpers/log_capture.h"
#include "helpers/renderer_fixture.h"
#include "helpers/rhi_fixture.h"
#include "helpers/runtime_fixture.h"
#include "rendering/frame_loop.h"
#include "rendering/render_system.h"
#include "rendering/ring_protocol.h"
#include "rendering/shared_frame_ring.h"
#include "simulation/gui_sink.h"
#include "simulation/simulation_thread.h"

#include "avis/graphics/frame_token.h"
#include "avis/graphics/gpu_completion_thread.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/shared.h"
#include "avis/runtime.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/snapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef AVIS_TEST_RENDERER_SHADER_DIRECTORY
#    error "tests/CMakeLists.txt defines AVIS_TEST_RENDERER_SHADER_DIRECTORY, where the renderer's shaders are built"
#endif

namespace
{
    namespace rendering = avis::visualizer::rendering;
    namespace vsim = avis::visualizer::simulation;
    using namespace std::chrono_literals;
    using avis::tests::log_capture;

    constexpr avis::graphics::texture_extent viewport_size{ .width = 64, .height = 36 };

    // Room for the simulation thread and the completion thread
    avis::runtime_configuration loop_runtime_configuration()
    {
        avis::runtime_configuration configuration = avis::tests::small_runtime_configuration(4);
        configuration.application_threads = 2;
        return configuration;
    }

    rendering::render_system_configuration warp_configuration()
    {
        const std::optional<avis::rhi::adapter_luid> warp = avis::tests::warp_luid();
        REQUIRE(warp.has_value());

        rendering::render_system_configuration configuration{
            .adapter = *warp,
            .allow_software_adapter = true,
            .settings = avis::tests::test_render_settings(),
            .shader_directory = std::filesystem::path{ AVIS_TEST_RENDERER_SHADER_DIRECTORY },
        };
        configuration.settings.debug_layer = true;
        configuration.settings.frames_in_flight = 2;
        return configuration;
    }

    // The demo scene's updates have nowhere to go
    class null_gui_sink final : public vsim::gui_sink
    {
    public:
        void tree_changed(std::vector<avis::scene::tree_change>) override {}
        void selected_entity_changed(std::optional<avis::scene::reflected_entity>) override {}
        void statistics_changed(const avis::scene::scene_statistics&) override {}
        void follow_target_changed(const vsim::follow_target&) override {}
    };

    class recording_frame_sink final : public rendering::frame_sink
    {
    public:
        void debug_information_changed(rendering::frame_debug_information information) override
        {
            const std::scoped_lock lock{ mutex_ };
            ++debug_updates_;
            last_debug_ = std::move(information);
        }

        void picks_completed(std::vector<rendering::pick_result> results) override
        {
            const std::scoped_lock lock{ mutex_ };
            picks_ += results.size();
        }

        void inspections_completed(std::vector<rendering::inspector_result>) override {}
        void screenshots_completed(std::vector<rendering::screenshot_result>) override {}

        void fatal_error(avis::error error) override
        {
            const std::scoped_lock lock{ mutex_ };
            fatal_ = std::move(error);
        }

        std::size_t debug_updates() const
        {
            const std::scoped_lock lock{ mutex_ };
            return debug_updates_;
        }

        std::optional<rendering::frame_debug_information> last_debug() const
        {
            const std::scoped_lock lock{ mutex_ };
            return last_debug_;
        }

        std::size_t picks() const
        {
            const std::scoped_lock lock{ mutex_ };
            return picks_;
        }

        std::optional<avis::error> fatal() const
        {
            const std::scoped_lock lock{ mutex_ };
            return fatal_;
        }

    private:
        mutable std::mutex mutex_;
        std::size_t debug_updates_ = 0;
        std::optional<rendering::frame_debug_information> last_debug_;
        std::size_t picks_ = 0;
        std::optional<avis::error> fatal_;
    };

    // Qt's render thread, as far as the ring's protocol goes (12 §3.3): on each wake it attaches to the current ring,
    // picks the newest frame and signals `consumed` with the pick's use. It samples nothing, so the signal comes from
    // the CPU at once. The modes stand in for a Qt that stops rendering (minimised, or stopped in a debugger).
    class fake_qt
    {
    public:
        enum class mode : std::uint8_t
        {
            render, // pick on wake, then signal consumed
            hold, // pick on wake, never signal: Qt still samples every slot it picked
            // The next pick isn't signalled, then stopped: a debugger break between render() and afterFrameEnd. The
            // renders before it were submitted with their signals, so this is the one use Qt still holds.
            break_after_pick,
            stopped // no more picks: the last one stays where it was
        };

        fake_qt(ID3D12Device& device, rendering::shared_frame_rings& rings) :
            device_{ device },
            rings_{ rings },
            thread_{ [this](const std::stop_token& token) { run(token); } }
        {}

        ~fake_qt()
        {
            thread_.request_stop();
            wake();
            thread_.join();
        }

        fake_qt(const fake_qt&) = delete;
        fake_qt& operator=(const fake_qt&) = delete;

        // The frame loop's wake (viewport_item::make_wake): any thread
        void wake()
        {
            {
                const std::scoped_lock lock{ mutex_ };
                woken_ = true;
            }
            condition_.notify_one();
        }

        void set_mode(const mode value)
        {
            const std::scoped_lock lock{ mutex_ };
            mode_ = value;
        }

        std::uint64_t picks() const
        {
            const std::scoped_lock lock{ mutex_ };
            return picks_;
        }

        // latest_completed's frame at each pick, in pick order
        std::vector<std::uint64_t> latest_seen() const
        {
            const std::scoped_lock lock{ mutex_ };
            return latest_seen_;
        }

        std::vector<std::uint64_t> frames_picked() const
        {
            const std::scoped_lock lock{ mutex_ };
            return frames_picked_;
        }

        // Errors found on the Qt side
        std::vector<std::string> errors() const
        {
            const std::scoped_lock lock{ mutex_ };
            return errors_;
        }

    private:
        void run(const std::stop_token& token)
        {
            while (!token.stop_requested())
            {
                mode current = mode::render;
                {
                    std::unique_lock lock{ mutex_ };
                    condition_.wait_for(lock, 50ms, [&] { return woken_ || token.stop_requested(); });
                    if (!woken_)
                    {
                        continue;
                    }
                    woken_ = false;
                    current = mode_;
                }
                if (current != mode::stopped)
                {
                    render(current);
                }
            }
        }

        // One QQuickRhiItemRenderer::render()
        void render(const mode current)
        {
            const std::shared_ptr<rendering::shared_ring_state> state = rings_.qt_current();
            if (state == nullptr)
            {
                return;
            }
            if (state != attached_)
            {
                if (!state->protocol.qt_attach())
                {
                    return; // already replaced: the next wake takes the newer one
                }
                avis::rhi::com_ptr<ID3D12Fence> consumed;
                if (FAILED(
                        device_.OpenSharedHandle(static_cast<HANDLE>(state->consumed.value), IID_PPV_ARGS(&consumed))))
                {
                    record_error("OpenSharedHandle failed on the ring's consumed fence");
                    return;
                }
                if (attached_ != nullptr)
                {
                    signal(attached_consumed_, attached_->protocol.qt_detach());
                }
                attached_ = state;
                attached_consumed_ = std::move(consumed);
            }

            const std::optional<rendering::qt_pick_result> pick = state->protocol.qt_pick();
            if (!pick)
            {
                return;
            }
            const std::optional<rendering::ring_frame> latest = state->protocol.latest_completed();
            {
                const std::scoped_lock lock{ mutex_ };
                ++picks_;
                frames_picked_.push_back(pick->frame);
                latest_seen_.push_back(latest ? latest->frame : 0);
            }
            if (current == mode::render)
            {
                signal(attached_consumed_, pick->use);
            }
            else if (current == mode::break_after_pick)
            {
                const std::scoped_lock lock{ mutex_ };
                mode_ = mode::stopped;
            }
        }

        void signal(const avis::rhi::com_ptr<ID3D12Fence>& fence, const std::uint64_t value)
        {
            if (FAILED(fence->Signal(value)))
            {
                record_error(std::format("signalling consumed with {} failed", value));
            }
        }

        void record_error(std::string message)
        {
            const std::scoped_lock lock{ mutex_ };
            errors_.push_back(std::move(message));
        }

        ID3D12Device& device_;
        rendering::shared_frame_rings& rings_;

        // The render thread's
        std::shared_ptr<rendering::shared_ring_state> attached_;
        avis::rhi::com_ptr<ID3D12Fence> attached_consumed_;

        mutable std::mutex mutex_;
        std::condition_variable condition_;
        bool woken_ = false;
        mode mode_ = mode::render;
        std::uint64_t picks_ = 0;
        std::vector<std::uint64_t> latest_seen_;
        std::vector<std::uint64_t> frames_picked_;
        std::vector<std::string> errors_;

        std::jthread thread_; // last: started once the rest exists
    };

    struct loop_options
    {
        float maximum_fps = 240.0f;
        std::size_t upload_memory_per_frame = rendering::render_system_configuration{}.upload_memory_per_frame;
    };

    // Members are destroyed in reverse: the loop first (it calls the fake Qt and reads the rings and snapshots), then
    // the fake Qt, the rings, the simulation thread, the completion thread, and the render system last
    class loop_fixture
    {
    public:
        explicit loop_fixture(const loop_options& options = {})
        {
            rendering::render_system_configuration configuration = warp_configuration();
            configuration.upload_memory_per_frame = options.upload_memory_per_frame;
            auto created = rendering::render_system::create(configuration);
            REQUIRE(created.has_value());
            system = std::move(*created);

            auto completions = avis::graphics::gpu_completion_thread::create(
                avis::runtime::thread_pool(), system->completion_driver());
            REQUIRE(completions.has_value());
            completion_thread = std::move(*completions);

            auto simulation = vsim::simulation_thread::start(
                avis::runtime::thread_pool(), assets, snapshots, gui, vsim::simulation_settings{ .demo_scene = true });
            REQUIRE(simulation.has_value());
            simulation_thread = std::move(*simulation);

            rings = std::make_unique<rendering::shared_frame_rings>(
                avis::rhi::sharing{ system->hardware_interface() },
                avis::rhi::resources{ system->hardware_interface() });
            rings->request_resize(viewport_size);

            qt = std::make_unique<fake_qt>(system->hardware_interface().state().device().native(), *rings);

            loop = std::make_unique<rendering::frame_loop>(
                *system,
                *rings,
                snapshots,
                avis::runtime::io_context(),
                sink,
                [qt = qt.get()]() { qt->wake(); },
                rendering::frame_loop_settings{ .maximum_fps = options.maximum_fps },
                rendering::render_options::from_settings(configuration.settings));
            loop->set_camera(
                rendering::look_at({ -45.0f, -45.0f, 35.0f }, Eigen::Vector3f::Zero(), Eigen::Vector3f::UnitZ()));
        }

        ~loop_fixture()
        {
            loop.reset();
            qt.reset();
            system->hardware_interface().wait_idle();
            rings.reset();
            simulation_thread.reset();
            completion_thread.reset();
            system.reset();
        }

        loop_fixture(const loop_fixture&) = delete;
        loop_fixture& operator=(const loop_fixture&) = delete;

        // Waits until `done`, failing the test after `timeout` with `what` and the loop's counters
        void wait_until(
            const std::string_view what, const std::function<bool()>& done, const std::chrono::seconds timeout = 60s)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!done())
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    const rendering::frame_loop_statistics counts = loop->statistics();
                    std::string ring_state = "no ring";
                    if (rendering::shared_frame_ring* const ring = rings->current(); ring != nullptr)
                    {
                        const rendering::ring_fence_values values = ring->fence_values();
                        const rendering::ring_protocol& protocol = ring->protocol();
                        const std::optional<rendering::ring_frame> latest = protocol.latest_completed();
                        ring_state = std::format(
                            "produced {}, consumed {}, showing {}, picked {} {} {}, latest frame {} in slot {}",
                            values.produced,
                            values.consumed,
                            protocol.showing() ? static_cast<int>(*protocol.showing()) : -1,
                            protocol.picked(0),
                            protocol.picked(1),
                            protocol.picked(2),
                            latest ? latest->frame : 0,
                            latest ? latest->slot : 0);
                    }
                    // Is a completion due that nothing polled?
                    avis::rhi::completion_driver driver = system->completion_driver();
                    const bool pending = driver.has_pending_work();
                    // Set: the completion thread isn't waiting on it (busy or blocked); clear: no wake came
                    const bool event_set =
                        WaitForSingleObject(static_cast<HANDLE>(driver.completion_event()), 0) == WAIT_OBJECT_0;
                    const std::size_t polled = driver.poll_completions();
                    std::this_thread::sleep_for(200ms);
                    ring_state += std::format(
                        "; pending work {}, event set {}, a poll completed {}, frames rendered after it {}",
                        pending,
                        event_set,
                        polled,
                        loop->statistics().frames_rendered);
                    FAIL(
                        std::format(
                            "timed out waiting for {}: {} ticks, {} rendered, {} published, {} skipped, {} failed, {} "
                            "wakes, {} Qt picks; ring: {}",
                            what,
                            counts.ticks,
                            counts.frames_rendered,
                            counts.frames_published,
                            counts.frames_skipped,
                            counts.frames_failed,
                            counts.wakes,
                            qt->picks(),
                            ring_state));
                }
                std::this_thread::sleep_for(2ms);
            }
        }

        void wait_for_frames(const std::uint64_t count)
        {
            wait_until(std::format("{} frames", count), [&] { return loop->statistics().frames_rendered >= count; });
        }

        avis::runtime runtime{ loop_runtime_configuration() };
        log_capture rhi_errors{ "rhi", 1024, spdlog::level::err };
        log_capture render_warnings{ "render", 1024, spdlog::level::warn };

        std::unique_ptr<rendering::render_system> system;
        std::unique_ptr<avis::graphics::gpu_completion_thread> completion_thread;

        avis::scene::asset_cache assets;
        avis::scene::snapshot_buffer snapshots;
        null_gui_sink gui;
        std::unique_ptr<vsim::simulation_thread> simulation_thread;

        std::unique_ptr<rendering::shared_frame_rings> rings;
        std::unique_ptr<fake_qt> qt;
        recording_frame_sink sink;
        std::unique_ptr<rendering::frame_loop> loop;
    };
} // namespace

TEST_CASE("frame loop: the tick period", "[visualizer][unit]")
{
    const rendering::frame_loop_settings defaults;
    CHECK(rendering::frame_loop::tick_interval(defaults, 0.0f, true) == 16'666'667ns); // the default 60 Hz
    CHECK(rendering::frame_loop::tick_interval(defaults, 144.0f, true) == 6'944'444ns); // the screen's
    CHECK(rendering::frame_loop::tick_interval(defaults, 144.0f, false) == 200ms); // hidden: 5 fps

    const rendering::frame_loop_settings capped{ .maximum_fps = 30.0f };
    CHECK(rendering::frame_loop::tick_interval(capped, 144.0f, true) == 33'333'333ns); // render.maximum_fps
    CHECK(rendering::frame_loop::tick_interval(capped, 144.0f, false) == 200ms);
}

TEST_CASE("frame loop: 100 frames of the demo scene reach the ring in order", "[visualizer]")
{
    loop_fixture fixture;
    fixture.loop->start();
    fixture.wait_for_frames(100);
    fixture.wait_until("20 picks", [&] { return fixture.qt->picks() >= 20; });

    // Every frame is published once its produced signal completes, and each publish found Qt idle or woke it
    fixture.wait_until("100 publishes", [&] { return fixture.loop->statistics().frames_published >= 100; });
    const rendering::frame_loop_statistics counts = fixture.loop->statistics();
    CHECK(counts.frames_skipped == 0);
    CHECK(counts.frames_failed == 0);
    CHECK(counts.idle_ticks == 0);
    CHECK(counts.wakes > 0);

    // latest_completed only grows, and each pick is that newest frame
    const std::vector<std::uint64_t> latest = fixture.qt->latest_seen();
    const std::vector<std::uint64_t> picked = fixture.qt->frames_picked();
    REQUIRE(latest.size() >= 20);
    for (std::size_t index = 1; index < latest.size(); ++index)
    {
        REQUIRE(latest[index] >= latest[index - 1]);
        REQUIRE(picked[index] > picked[index - 1]); // Qt is woken only for a new frame
    }
    CHECK(latest.back() > latest.front());

    // The renderer saw the demo scene's snapshots, and the debug information arrived at about 4 Hz
    fixture.wait_until("debug information", [&] { return fixture.sink.debug_updates() >= 2; });
    const std::optional<rendering::frame_debug_information> debug = fixture.sink.last_debug();
    REQUIRE(debug.has_value());
    CHECK(debug->renderer.last_publish_sequence > 1);
    CHECK(debug->renderer.views == 1);
    CHECK(debug->renderer.graph == nullptr);
    CHECK_FALSE(debug->passes.empty());
    CHECK_FALSE(debug->resources.empty());

    fixture.loop->stop();
    CHECK(fixture.qt->errors().empty());
    CHECK(fixture.rhi_errors.lines().empty());
    CHECK(fixture.render_warnings.lines().empty());
    CHECK_FALSE(fixture.sink.fatal().has_value());
}

TEST_CASE("frame loop: the tick rate follows render.maximum_fps", "[visualizer]")
{
    constexpr float fps = 20.0f;
    loop_fixture fixture{ { .maximum_fps = fps } };
    fixture.loop->start();
    fixture.wait_for_frames(5); // past the first frames' environment bake

    const auto first_time = std::chrono::steady_clock::now();
    const std::uint64_t first_ticks = fixture.loop->statistics().ticks;
    std::this_thread::sleep_for(2s);
    const std::uint64_t last_ticks = fixture.loop->statistics().ticks;
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - first_time;

    const double rate = static_cast<double>(last_ticks - first_ticks) / elapsed.count();
    INFO(std::format("{:.2f} ticks per second", rate));
    CHECK(rate >= 0.9 * fps);
    CHECK(rate <= 1.1 * fps);
    CHECK(fixture.loop->statistics().frames_rendered + 3 >= last_ticks); // every tick rendered a frame
}

TEST_CASE("frame loop: a Qt side that stops rendering doesn't stall it", "[visualizer]")
{
    loop_fixture fixture;
    fixture.loop->start();
    fixture.wait_for_frames(10);

    // Qt picks a frame and stops in the middle of sampling it: its `consumed` never comes (a debugger break). Avis goes
    // on with the other two slots.
    const std::uint64_t picks = fixture.qt->picks();
    fixture.qt->set_mode(fake_qt::mode::break_after_pick);
    fixture.wait_until("the pick before the break", [&] { return fixture.qt->picks() > picks; });

    const std::uint64_t rendered = fixture.loop->statistics().frames_rendered;
    fixture.wait_for_frames(rendered + 30);
    CHECK(fixture.loop->statistics().frames_published >= rendered + 20);

    // Back: Qt gets a newer frame as soon as it's woken
    const std::uint64_t last_picked = fixture.qt->frames_picked().back();
    fixture.qt->set_mode(fake_qt::mode::render);
    fixture.qt->wake();
    fixture.wait_until("Qt to pick again", [&] { return fixture.qt->frames_picked().back() > last_picked + 20; });

    fixture.loop->stop();
    CHECK(fixture.qt->errors().empty());
    CHECK(fixture.rhi_errors.lines().empty());
}

TEST_CASE("frame loop: running out of frame memory skips frames, then it recovers", "[visualizer]")
{
    // A frame of one view uses 12 to 14 KiB of constants on WARP, and 64 debug blits more than 20 KiB (measured)
    loop_fixture fixture{ { .upload_memory_per_frame = 16 * 1024 } };
    fixture.loop->start();
    fixture.wait_for_frames(5);
    CHECK(fixture.loop->statistics().frames_skipped == 0);

    std::vector<rendering::debug_selection> blits(64, rendering::debug_selection{ .resource = "gbuffer_albedo" });
    fixture.loop->set_debug_blits(blits);
    fixture.wait_until("skipped frames", [&] { return fixture.loop->statistics().frames_skipped >= 5; });

    // The skipped frames submitted nothing and ended themselves (D-98, D-102); without the blits the chain goes on
    fixture.loop->set_debug_blits({});
    const std::uint64_t rendered = fixture.loop->statistics().frames_rendered;
    fixture.wait_for_frames(rendered + 10);
    const std::uint64_t skipped = fixture.loop->statistics().frames_skipped;
    fixture.wait_for_frames(rendered + 20);
    CHECK(fixture.loop->statistics().frames_skipped <= skipped + 1);
    CHECK(fixture.loop->statistics().frames_failed == 0);

    // One warning at most per second names the budget
    bool named = false;
    for (const std::string& line : fixture.render_warnings.lines())
    {
        named = named || line.find("upload_memory_per_frame") != std::string::npos;
    }
    CHECK(named);
    fixture.loop->stop();
    CHECK(fixture.rhi_errors.lines().empty());
    CHECK_FALSE(fixture.sink.fatal().has_value());
}

TEST_CASE("frame loop: stopping drains the scope and leaves no frame open", "[visualizer]")
{
    loop_fixture fixture;
    fixture.loop->start();
    fixture.wait_for_frames(10);

    SECTION("while it renders")
    {
        fixture.loop->stop();
    }

    SECTION("while its gate waits for Qt")
    {
        // Qt holds every slot it picks: after three picks no slot is free, and the gate waits on `consumed`
        fixture.qt->set_mode(fake_qt::mode::hold);
        fixture.wait_until(
            "the gate to wait",
            [&]
            {
                const rendering::frame_loop_statistics before = fixture.loop->statistics();
                std::this_thread::sleep_for(200ms);
                return fixture.loop->statistics().frames_rendered == before.frames_rendered;
            });
        fixture.loop->stop();
    }

    const std::uint64_t rendered = fixture.loop->statistics().frames_rendered;
    std::this_thread::sleep_for(100ms);
    CHECK(fixture.loop->statistics().frames_rendered == rendered); // nothing re-armed
    fixture.loop->stop(); // idempotent

    // No token is left open: the scheduler starts a frame again (it asserts when one is outstanding)
    fixture.system->hardware_interface().wait_idle();
    std::optional<avis::graphics::frame_token> frame = fixture.system->frames().try_begin_frame();
    CHECK(frame.has_value());
    frame.reset();
    CHECK(fixture.rhi_errors.lines().empty());
}
