// Cross-device sharing: shared textures and fences (docs/tasks/06-rhi.md §6.11). Two RHIs in one process stand in for
// the RHI and Qt's device; both pick the same adapter.

#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"

#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/d3d12/native_access.h"
#include "avis/rhi/error.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/shared.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

using namespace avis;
using avis::tests::started_wait;
using avis::tests::started_waits;
using rhi::format;
using rhi::queue_type;
using rhi::texture_usage;

namespace
{
    // Spins until the fence reaches `value`, without completing anything on the RHI
    void wait_for_value(const rhi::shared_fence& fence, const std::uint64_t value)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 10 };
        while (fence.completed_value() < value)
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }

    rhi::texture_description shared_texture_description()
    {
        return { .width = 64,
                 .height = 64,
                 .format = format::rgba8_unorm,
                 .usage = texture_usage::shader_resource | texture_usage::render_target | texture_usage::shared,
                 .debug_name = "shared texture" };
    }
} // namespace

TEST_CASE("A fence exported by one RHI is signalled by another", "[rhi]")
{
    started_waits waits;
    tests::test_rhi exporter;
    tests::test_rhi importer;
    rhi::sharing exporter_sharing{ *exporter };
    rhi::sharing importer_sharing{ *importer };
    rhi::completion_driver exporter_driver{ *exporter };

    auto texture = exporter_sharing.create_shared_texture(shared_texture_description());
    REQUIRE(texture.has_value());
    auto texture_handle = exporter_sharing.export_shared_handle(*texture);
    REQUIRE(texture_handle.has_value());
    CHECK(texture_handle->value != nullptr);

    auto fence = exporter_sharing.create_shared_fence();
    REQUIRE(fence.has_value());
    auto fence_handle = exporter_sharing.export_shared_handle(*fence);
    REQUIRE(fence_handle.has_value());
    REQUIRE(fence_handle->value != nullptr);

    // The importer opens both, as Qt does
    ID3D12Resource* opened_texture = nullptr;
    REQUIRE(SUCCEEDED(importer->state().device().native().OpenSharedHandle(
        static_cast<HANDLE>(texture_handle->value), IID_PPV_ARGS(&opened_texture))));
    opened_texture->Release();

    auto imported = importer_sharing.import_shared_fence(*fence_handle);
    REQUIRE(imported.has_value());
    CHECK(fence->completed_value() == 0);

    const started_wait& first = waits.start(exporter_sharing.when_reached(*fence, 1));
    const started_wait& second = waits.start(exporter_sharing.when_reached(*fence, 2));
    CHECK(exporter_driver.has_pending_work());
    CHECK(exporter_driver.poll_completions() == 0);

    rhi::queue<queue_type::graphics> importer_queue{ *importer };
    importer_queue.signal(*imported, 1);
    wait_for_value(*fence, 1);
    CHECK(fence->completed_value() == 1);
    CHECK(imported->completed_value() == 1);
    CHECK(exporter_driver.poll_completions() == 1);
    CHECK(first.counts.values == 1);
    CHECK(second.counts.total() == 0);

    importer_queue.signal(*imported, 2);
    wait_for_value(*fence, 2);
    CHECK(exporter_driver.poll_completions() == 1);
    CHECK(second.counts.values == 1);
    CHECK_FALSE(exporter_driver.has_pending_work()); // the fence's last waiter detached it

    // A reached value completes inline
    const started_wait& reached = waits.start(exporter_sharing.when_reached(*fence, 2));
    CHECK(reached.counts.values == 1);
    CHECK(reached.counts.thread == std::this_thread::get_id());

    // A queue of the exporter waits on the importer's signal, then signals the fence back
    rhi::queue<queue_type::compute> exporter_queue{ *exporter };
    exporter_queue.wait(*fence, 2);
    exporter_queue.signal(*fence, 3);
    wait_for_value(*imported, 3);

    importer->wait_idle();
    exporter->wait_idle();
    rhi::resources{ *exporter }.destroy(*texture);
}

TEST_CASE("Shared textures are committed with the SHARED heap flag and export one handle", "[rhi]")
{
    tests::test_rhi hardware_interface;
    rhi::sharing sharing{ *hardware_interface };

    auto texture = sharing.create_shared_texture(shared_texture_description());
    REQUIRE(texture.has_value());

    ID3D12Resource* const resource = rhi::d3d12::native_resource(*hardware_interface, *texture);
    REQUIRE(resource != nullptr);
    D3D12_HEAP_PROPERTIES properties{};
    D3D12_HEAP_FLAGS flags{};
    REQUIRE(SUCCEEDED(resource->GetHeapProperties(&properties, &flags)));
    CHECK((flags & D3D12_HEAP_FLAG_SHARED) != 0);

    auto first = sharing.export_shared_handle(*texture);
    auto second = sharing.export_shared_handle(*texture);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->value == second->value);

    auto fence = sharing.create_shared_fence(7);
    REQUIRE(fence.has_value());
    CHECK(fence->completed_value() == 7);
    auto fence_first = sharing.export_shared_handle(*fence);
    auto fence_second = sharing.export_shared_handle(*fence);
    REQUIRE(fence_first.has_value());
    REQUIRE(fence_second.has_value());
    CHECK(fence_first->value == fence_second->value);

    rhi::texture_description plain = shared_texture_description();
    plain.usage = texture_usage::shader_resource;
    const auto refused = sharing.create_shared_texture(plain);
    REQUIRE_FALSE(refused.has_value());
    CHECK(rhi::is_error(refused.error(), rhi::error_kind::invalid_argument));

    rhi::resources{ *hardware_interface }.destroy(*texture);
}

TEST_CASE("A shared fence without waiters isn't polled, and its destruction waits for a queued signal", "[rhi]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    rhi::sharing sharing{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };
    const rhi::d3d12::deferred_release& releases = hardware_interface->state().lifetime().releases();

    std::optional<rhi::shared_fence> fence;
    {
        auto created = sharing.create_shared_fence();
        REQUIRE(created.has_value());
        fence.emplace(std::move(*created));
    }
    CHECK_FALSE(driver.has_pending_work());

    // The signal waits behind the gate, so it is still in flight when the fence is destroyed
    rhi::queue<queue_type::graphics> queue{ *hardware_interface };
    gate.point(queue_type::graphics, 1);
    queue.signal(*fence, 5);
    const rhi::synchronization_point signal_point = queue.last_submitted();
    fence.reset();
    driver.poll_completions();
    CHECK(releases.pending() == 1);

    gate.open(1);
    hardware_interface.wait_for(signal_point);
    CHECK(releases.pending() == 0);
    CHECK_FALSE(driver.has_pending_work());
}

TEST_CASE("A stop request removes a pending when_reached", "[rhi]")
{
    started_waits waits;
    tests::test_rhi hardware_interface;
    rhi::sharing sharing{ *hardware_interface };
    rhi::completion_driver driver{ *hardware_interface };

    auto fence = sharing.create_shared_fence();
    REQUIRE(fence.has_value());

    SECTION("after start")
    {
        started_wait& wait = waits.start(sharing.when_reached(*fence, 1));
        CHECK(driver.has_pending_work());
        wait.source.request_stop();
        CHECK(wait.counts.stops == 1);
        CHECK(wait.counts.thread == std::this_thread::get_id());
        CHECK_FALSE(driver.has_pending_work());
    }

    SECTION("before start")
    {
        started_wait& wait = waits.connect(sharing.when_reached(*fence, 1));
        wait.source.request_stop();
        stdexec::start(wait.operation);
        CHECK(wait.counts.stops == 1);
        CHECK_FALSE(driver.has_pending_work());
    }

    SECTION("one of two waiters")
    {
        started_wait& stopped = waits.start(sharing.when_reached(*fence, 1));
        const started_wait& kept = waits.start(sharing.when_reached(*fence, 1));
        stopped.source.request_stop();
        CHECK(stopped.counts.stops == 1);
        CHECK(driver.has_pending_work());

        rhi::queue<queue_type::copy> queue{ *hardware_interface };
        queue.signal(*fence, 1);
        wait_for_value(*fence, 1);
        CHECK(driver.poll_completions() == 1);
        CHECK(kept.counts.values == 1);
        CHECK_FALSE(driver.has_pending_work());
    }

    hardware_interface->wait_idle();
}
