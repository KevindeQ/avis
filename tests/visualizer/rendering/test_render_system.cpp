// The Visualizer's graphics objects (docs/tasks/12-visualizer.md §12.3): the full set created from an adapter LUID,
// frames driven through the completion event the way the Visualizer's Qt notifier drives them, and a shutdown that
// leaves no live D3D12 object.

#include "helpers/log_capture.h"
#include "helpers/renderer_fixture.h"
#include "helpers/rhi_fixture.h"
#include "helpers/runtime_fixture.h"
#include "rendering/render_system.h"
#include "rendering/scene_renderer.h"

#include "avis/graphics/frame_token.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/error.h"
#include "avis/rhi/synchronization.h"
#include "avis/scene/assets/asset_cache.h"
#include "avis/scene/scene.h"
#include "avis/scene/snapshot.h"
#include "avis/threading/parallel_helpers.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifndef AVIS_TEST_RENDERER_SHADER_DIRECTORY
#    error "tests/CMakeLists.txt defines AVIS_TEST_RENDERER_SHADER_DIRECTORY, where the renderer's shaders are built"
#endif

namespace
{
    namespace rendering = avis::visualizer::rendering;
    using avis::tests::log_capture;

    constexpr int frame_count = 10;

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

    // The Visualizer's QWinEventNotifier, as a test loop: waits on the completion event and polls until `done`
    void drive_until(avis::rhi::completion_driver driver, const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 30 };
        while (!done())
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(driver.completion_event()), 1'000);
            REQUIRE((waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT));
            driver.poll_completions();
        }
    }

    // Receives begin_frame's token
    struct frame_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value(avis::graphics::frame_token token) noexcept
        {
            frame->emplace(std::move(token));
        }

        void set_error(avis::error error) noexcept
        {
            failure->emplace(std::move(error));
        }

        void set_stopped() noexcept
        {
            failure->emplace(avis::error{ .message = "stopped" });
        }

        stdexec::env<> get_env() const noexcept
        {
            return {};
        }

        std::optional<avis::graphics::frame_token>* frame;
        std::optional<avis::error>* failure;
    };

    struct wait_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        void set_value() noexcept
        {
            *done = true;
        }

        void set_error(avis::error error) noexcept
        {
            failure->emplace(std::move(error));
            *done = true;
        }

        void set_stopped() noexcept
        {
            failure->emplace(avis::error{ .message = "stopped" });
            *done = true;
        }

        stdexec::env<> get_env() const noexcept
        {
            return {};
        }

        bool* done;
        std::optional<avis::error>* failure;
    };

    // The debug layer's live-object report for a device the caller holds the last reference to: one line per live
    // object other than the device itself
    std::vector<std::string> live_objects(ID3D12Device& device)
    {
        avis::rhi::com_ptr<ID3D12InfoQueue> info_queue;
        avis::rhi::com_ptr<ID3D12DebugDevice> debug_device;
        REQUIRE(SUCCEEDED(device.QueryInterface(IID_PPV_ARGS(&info_queue))));
        REQUIRE(SUCCEEDED(device.QueryInterface(IID_PPV_ARGS(&debug_device))));

        info_queue->ClearStoredMessages();
        REQUIRE(SUCCEEDED(info_queue->PushEmptyStorageFilter())); // whatever the RHI filtered
        REQUIRE(SUCCEEDED(debug_device->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL)));

        std::vector<std::string> live;
        bool device_listed = false;
        for (UINT64 index = 0; index < info_queue->GetNumStoredMessages(); ++index)
        {
            SIZE_T size = 0;
            REQUIRE(SUCCEEDED(info_queue->GetMessage(index, nullptr, &size)));
            std::vector<std::byte> storage(size);
            auto* const message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            REQUIRE(SUCCEEDED(info_queue->GetMessage(index, message, &size)));
            if (message->ID == D3D12_MESSAGE_ID_LIVE_DEVICE)
            {
                device_listed = true;
            }
            else if (message->ID != D3D12_MESSAGE_ID_LIVE_OBJECT_SUMMARY)
            {
                live.emplace_back(message->pDescription, message->DescriptionByteLength - 1);
            }
        }
        info_queue->PopStorageFilter();
        // The report did run: it always lists the device
        CHECK(device_listed);
        return live;
    }
} // namespace

TEST_CASE_METHOD(
    avis::tests::runtime_fixture,
    "the render system starts on a given adapter, runs frames from the completion event and shuts down cleanly",
    "[visualizer]")
{
    const log_capture rhi_errors{ "rhi", 1024, spdlog::level::err };
    const rendering::render_system_configuration configuration = warp_configuration();

    auto created = rendering::render_system::create(configuration);
    REQUIRE(created.has_value());
    std::unique_ptr<rendering::render_system> system = std::move(*created);

    // On the adapter it was given (12 §3.2 step 3)
    CHECK(system->capabilities().luid == *configuration.adapter);
    CHECK(system->capabilities().software_adapter);
    CHECK(system->frames().frames_in_flight() == 2);

    // The extra reference keeps the device alive after the RHI, for the live-object report
    avis::rhi::com_ptr<ID3D12Device> device{ &system->hardware_interface().state().device().native() };

    // An empty scene's snapshot
    avis::scene::asset_cache assets;
    avis::scene::scene world{ assets };
    avis::scene::snapshot_buffer snapshots;
    world.update_world_transforms();
    world.make_snapshot(snapshots.begin_write());
    snapshots.publish();
    world.end_tick();

    // Frames as the frame chain will run them (12.7): begin_frame completes from the completion event, then the
    // profiler's prepare_frame, the renderer records the frame with no view, and end_frame. With two frames in flight,
    // frames 3 to 10 wait for the GPU through the event.
    const avis::rhi::completion_driver driver = system->completion_driver();
    std::vector<avis::rhi::synchronization_point> last_submissions;
    for (int index = 0; index < frame_count; ++index)
    {
        std::optional<avis::graphics::frame_token> frame;
        std::optional<avis::error> failure;
        auto operation = stdexec::connect(system->frames().begin_frame(), frame_receiver{ &frame, &failure });
        stdexec::start(operation);
        drive_until(driver, [&] { return frame.has_value() || failure.has_value(); });
        REQUIRE_FALSE(failure.has_value());

        system->profiler().prepare_frame(*frame);
        system->renderer().retire(frame->completed_index());
        const avis::scene::snapshot_reference snapshot = snapshots.acquire();
        REQUIRE(static_cast<bool>(snapshot));
        auto submitted = avis::threading::sync_wait_result(
            system->renderer().render(*frame, *snapshot, {}, std::chrono::steady_clock::now()));
        if (!submitted)
        {
            FAIL(std::format("frame {} failed: {}", index, submitted.error()));
        }
        const std::span<const avis::rhi::synchronization_point> points = submitted->last_submissions();
        last_submissions.assign(points.begin(), points.end());
        system->frames().end_frame(std::move(*frame), points);
    }

    // The last frame completes through the event too, so nothing is left for the shutdown's wait_idle
    {
        bool done = false;
        std::optional<avis::error> failure;
        auto operation = stdexec::connect(
            system->synchronization().when_complete(std::span{ last_submissions }), wait_receiver{ &done, &failure });
        stdexec::start(operation);
        drive_until(driver, [&] { return done; });
        REQUIRE_FALSE(failure.has_value());
    }
    CHECK_FALSE(driver.has_pending_work());

    // The shutdown of 12 §3.4: the graphics objects in reverse, then the RHI
    system.reset();
    CHECK(rhi_errors.lines().empty());

    const std::vector<std::string> live = live_objects(*device.get());
    for (const std::string& object : live)
    {
        UNSCOPED_INFO(object);
    }
    CHECK(live.empty());
}

TEST_CASE_METHOD(avis::tests::runtime_fixture, "the render system names the startup step that failed", "[visualizer]")
{
    SECTION("an adapter that doesn't exist")
    {
        rendering::render_system_configuration configuration = warp_configuration();
        configuration.adapter = avis::rhi::adapter_luid{ .low = 0xdeadbeef, .high = 0x7eadbeef };

        const auto created = rendering::render_system::create(configuration);
        REQUIRE_FALSE(created.has_value());
        CHECK(created.error().message.starts_with("the graphics device could not be created: "));
    }

    SECTION("a shader directory without the renderer's shaders")
    {
        rendering::render_system_configuration configuration = warp_configuration();
        configuration.shader_directory = std::filesystem::path{ AVIS_TEST_RENDERER_SHADER_DIRECTORY } / "missing";

        const auto created = rendering::render_system::create(configuration);
        REQUIRE_FALSE(created.has_value());
        CHECK(created.error().message.starts_with("the renderer's shaders could not be loaded: "));
        CHECK(created.error().message.find("manifest.yaml") != std::string::npos);
    }
}
