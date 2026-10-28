// Open-list epochs and deferred release (docs/tasks/06-rhi.md §6.5). The epoch and release_entry tests need no device;
// the lifetime manager tests run on a real device with test payloads, since no resources exist yet.

#include "helpers/rhi_fixture.h"
#include "helpers/rhi_waits.h"

#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/deferred_release.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/d3d12/lifetime_manager.h"
#include "avis/rhi/d3d12/open_list_epochs.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace avis;
using rhi::d3d12::lifetime_manager;
using rhi::d3d12::open_list_epochs;
using rhi::d3d12::release_entry;

namespace
{
    // Move-only payload that counts its destruction
    class release_probe
    {
    public:
        explicit release_probe(std::atomic<int>& freed) noexcept : freed_{ &freed } {}

        release_probe(release_probe&& other) noexcept : freed_{ std::exchange(other.freed_, nullptr) } {}
        release_probe& operator=(release_probe&&) = delete;
        release_probe(const release_probe&) = delete;
        release_probe& operator=(const release_probe&) = delete;

        ~release_probe()
        {
            if (freed_ != nullptr)
            {
                ++*freed_;
            }
        }

    private:
        std::atomic<int>* freed_;
    };

    bool event_signalled(void* const event)
    {
        return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    }
} // namespace

TEST_CASE("open_list_epochs: a release parked while a list is open waits for that list", "[rhi][unit]")
{
    open_list_epochs epochs;

    const std::uint64_t open_list = epochs.enter();
    const std::uint64_t parked = epochs.current();

    // Later lists come and go; the first one stays open
    for (int round = 0; round < 10; ++round)
    {
        const std::uint64_t other = epochs.enter();
        epochs.try_advance();
        epochs.leave(other);
        CHECK(epochs.try_advance() <= parked);
    }

    epochs.leave(open_list);
    CHECK(epochs.try_advance() > parked);
    CHECK(epochs.open_lists() == 0);
}

TEST_CASE("open_list_epochs: with no list open, a release is clean at once", "[rhi][unit]")
{
    open_list_epochs epochs;
    const std::uint64_t parked = epochs.current();
    CHECK(epochs.try_advance() > parked);
}

TEST_CASE("open_list_epochs: concurrent enter and leave leave the counters at 0", "[rhi][unit]")
{
    open_list_epochs epochs;
    std::atomic<bool> done{ false };
    std::thread advancer{ [&]
                          {
                              while (!done.load())
                              {
                                  epochs.try_advance();
                              }
                          } };

    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; ++thread)
    {
        threads.emplace_back(
            [&]
            {
                for (int pair = 0; pair < 100'000; ++pair)
                {
                    epochs.leave(epochs.enter());
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    done = true;
    advancer.join();

    CHECK(epochs.open_lists() == 0);
    const std::uint64_t parked = epochs.current();
    CHECK(epochs.try_advance() > parked);
}

TEST_CASE("release_entry destroys its payload exactly once", "[rhi][unit]")
{
    std::atomic<int> freed{ 0 };
    {
        release_entry entry{ release_probe{ freed } };
        release_entry moved{ std::move(entry) };
        CHECK(freed == 0);

        std::vector<release_entry> entries;
        entries.push_back(std::move(moved));
        entries.reserve(64); // moves the entries again
        CHECK(freed == 0);
    }
    CHECK(freed == 1);

    // Any nothrow-movable payload, unique_ptrs to non-movable states included
    struct counted_state
    {
        explicit counted_state(std::atomic<int>& counter) noexcept : freed{ counter } {}
        ~counted_state()
        {
            ++freed;
        }
        std::atomic<int>& freed;
    };
    {
        release_entry entry{ std::make_unique<counted_state>(freed) };
    }
    CHECK(freed == 2);
}

TEST_CASE("Lifetime manager: an entry destroyed with no list open waits for its tag", "[rhi]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    lifetime_manager& lifetime = hardware_interface->state().lifetime();
    rhi::completion_driver driver{ *hardware_interface };
    std::atomic<int> freed{ 0 };

    // In flight on the graphics queue when the object is destroyed
    const rhi::synchronization_point point = gate.point(rhi::queue_type::graphics, 1);

    lifetime.destroy(release_entry{ release_probe{ freed } });
    CHECK(lifetime.releases().parked() == 0);
    CHECK(lifetime.releases().tagged() == 1); // tagged at once: no list was open
    CHECK(driver.has_pending_work());

    driver.poll_completions();
    CHECK(freed == 0);

    gate.open(1);
    hardware_interface.wait_for(point);
    CHECK(freed == 1);
    CHECK_FALSE(driver.has_pending_work());
}

TEST_CASE("Lifetime manager: an entry destroyed while a list is open waits for the list, then for its tag", "[rhi]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    lifetime_manager& lifetime = hardware_interface->state().lifetime();
    rhi::completion_driver driver{ *hardware_interface };
    std::atomic<int> freed{ 0 };

    const std::uint64_t open_list = lifetime.list_opened();
    lifetime.destroy(release_entry{ release_probe{ freed } });
    CHECK(lifetime.releases().parked() == 1);

    // Other work completes; the entry stays parked while the list is open
    hardware_interface.wait_for(tests::bare_signal(hardware_interface, rhi::queue_type::copy));
    driver.poll_completions();
    CHECK(lifetime.releases().parked() == 1);
    CHECK(freed == 0);

    // The open list is submitted (gated, so it doesn't finish yet), then closed
    const rhi::synchronization_point submitted = gate.point(rhi::queue_type::compute, 1);
    lifetime.list_closed(open_list);
    driver.poll_completions();
    CHECK(lifetime.releases().parked() == 0);
    CHECK(lifetime.releases().tagged() == 1);
    CHECK(freed == 0);

    gate.open(1);
    hardware_interface.wait_for(submitted);
    CHECK(freed == 1);
    CHECK_FALSE(driver.has_pending_work());
}

TEST_CASE("Lifetime manager: poll_completions frees at most 256 entries per call", "[rhi]")
{
    tests::test_rhi hardware_interface;
    lifetime_manager& lifetime = hardware_interface->state().lifetime();
    rhi::completion_driver driver{ *hardware_interface };
    void* const event = driver.completion_event();
    std::atomic<int> freed{ 0 };

    for (int index = 0; index < 1000; ++index)
    {
        lifetime.destroy(release_entry{ release_probe{ freed } }); // nothing submitted: ready at once
    }
    CHECK_FALSE(event_signalled(event));

    driver.poll_completions();
    CHECK(freed == 256);
    CHECK(event_signalled(event)); // signalled again: the driver should call back soon

    driver.poll_completions();
    driver.poll_completions();
    CHECK(freed == 768);
    CHECK(driver.has_pending_work());
    CHECK(event_signalled(event)); // from the third call
    driver.poll_completions();
    CHECK(freed == 1000);
    CHECK_FALSE(driver.has_pending_work());
    CHECK_FALSE(event_signalled(event));
}

TEST_CASE("Lifetime manager: drain frees everything after wait_idle", "[rhi]")
{
    tests::test_rhi hardware_interface;
    tests::rhi_gate gate{ hardware_interface };
    lifetime_manager& lifetime = hardware_interface->state().lifetime();
    rhi::completion_driver driver{ *hardware_interface };
    std::atomic<int> freed{ 0 };

    gate.point(rhi::queue_type::copy, 1);
    for (int index = 0; index < 300; ++index)
    {
        lifetime.destroy(release_entry{ release_probe{ freed } });
    }
    driver.poll_completions();
    CHECK(freed == 0);
    CHECK(driver.has_pending_work());

    gate.open(1);
    hardware_interface->wait_idle(); // one poll_completions: 256 entries
    CHECK(freed == 256);
    CHECK(driver.has_pending_work());

    lifetime.drain();
    CHECK(freed == 300);
    CHECK_FALSE(driver.has_pending_work());
}

TEST_CASE("Destroying the RHI frees releases that are still pending", "[rhi]")
{
    std::atomic<int> freed{ 0 };
    {
        tests::test_rhi hardware_interface;
        lifetime_manager& lifetime = hardware_interface->state().lifetime();
        const std::uint64_t open_list = lifetime.list_opened();
        lifetime.destroy(release_entry{ release_probe{ freed } });
        lifetime.list_closed(open_list);
        CHECK(freed == 0);
    }
    CHECK(freed == 1);
}
