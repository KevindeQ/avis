// The shared frame ring (docs/tasks/12-visualizer.md §12.4): ring_protocol on the CPU, then the ring on the RHI
// fixture with a stand-in for Qt that has its own queue, command list and fence, and opens the ring's handles with
// OpenSharedHandle as ui/viewport_renderer will (12 §3.2 step 5).

#include "helpers/rhi_fixture.h"
#include "rendering/ring_protocol.h"
#include "rendering/shared_frame_ring.h"

#include "avis/rhi/barriers.h"
#include "avis/rhi/command_list.h"
#include "avis/rhi/completion_driver.h"
#include "avis/rhi/d3d12/com_ptr.h"
#include "avis/rhi/d3d12/d3d12.h"
#include "avis/rhi/d3d12/device.h"
#include "avis/rhi/d3d12/device_state.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/shared.h"

#include <stdexec/execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <variant>
#include <vector>

using namespace avis::visualizer::rendering;

namespace
{
    namespace rhi = avis::rhi;

    // ---- CPU: the protocol with fence values the test sets ----

    struct cpu_ring
    {
        ring_protocol protocol;
        ring_fence_values fences;
        std::uint64_t next_frame = 0;

        // Acquires a slot, writes a frame into it, completes it on the "GPU" and publishes it
        std::uint32_t frame()
        {
            const slot_acquisition acquired = protocol.try_acquire_slot(fences);
            REQUIRE(acquired.slot.has_value());
            const std::uint64_t frame = ++next_frame;
            protocol.bind(*acquired.slot, frame);
            fences.produced = frame;
            protocol.publish(frame, *acquired.slot);
            return *acquired.slot;
        }

        qt_pick_result pick()
        {
            const std::optional<qt_pick_result> pick = protocol.qt_pick();
            REQUIRE(pick.has_value());
            return *pick;
        }
    };
} // namespace

TEST_CASE("ring_protocol: Avis never gets the slot Qt is showing", "[visualizer][unit]")
{
    cpu_ring ring;
    std::optional<qt_pick_result> previous;
    for (int index = 0; index < 50; ++index)
    {
        const std::uint32_t slot = ring.frame();
        if (previous)
        {
            CHECK(slot != previous->slot);
            ring.fences.consumed = previous->use; // Qt's previous render is done
        }
        previous = ring.pick();
        CHECK(previous->slot == slot);
        CHECK(ring.protocol.showing() == slot);
    }
}

TEST_CASE("ring_protocol: with Qt holding two slots, the third is free", "[visualizer][unit]")
{
    cpu_ring ring;
    std::deque<qt_pick_result> held; // Qt's renders not consumed yet: the one showing and the one before
    for (int index = 0; index < 100; ++index)
    {
        const std::uint32_t slot = ring.frame(); // requires a free slot
        for (const qt_pick_result& pick : held)
        {
            CHECK(slot != pick.slot);
        }
        held.push_back(ring.pick());
        if (held.size() > 2)
        {
            ring.fences.consumed = held.front().use;
            held.pop_front();
        }
    }
}

TEST_CASE("ring_protocol: when Qt stops picking, Avis alternates between the other two slots", "[visualizer][unit]")
{
    cpu_ring ring;
    ring.frame();
    const qt_pick_result shown = ring.pick();
    ring.fences.consumed = shown.use;

    std::vector<std::uint32_t> slots;
    for (int index = 0; index < 10; ++index)
    {
        slots.push_back(ring.frame());
    }
    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        CHECK(slots[index] != shown.slot);
        if (index > 0)
        {
            CHECK(slots[index] != slots[index - 1]);
        }
    }
    CHECK(ring.protocol.showing() == shown.slot);
}

TEST_CASE("ring_protocol: the slot released longest ago is preferred", "[visualizer][unit]")
{
    cpu_ring ring;
    // Three frames, each shown by Qt and consumed: slots 0, 1 and 2 in turn (a never-written slot comes first)
    for (std::uint32_t expected = 0; expected < 3; ++expected)
    {
        CHECK(ring.frame() == expected);
        ring.fences.consumed = ring.pick().use;
    }
    // Slot 2 is showing. Slot 0 was released before slot 1.
    const slot_acquisition acquired = ring.protocol.try_acquire_slot(ring.fences);
    REQUIRE(acquired.slot.has_value());
    CHECK(*acquired.slot == 0);
}

TEST_CASE("ring_protocol: the waits when no slot is free", "[visualizer][unit]")
{
    cpu_ring ring;

    SECTION("Avis's own frames in flight: the oldest on produced")
    {
        ring.frame(); // frame 1, the latest
        for (const std::uint64_t frame : { 2, 3 })
        {
            const slot_acquisition acquired = ring.protocol.try_acquire_slot(ring.fences);
            REQUIRE(acquired.slot.has_value());
            ring.protocol.bind(*acquired.slot, frame);
        }
        const slot_acquisition none = ring.protocol.try_acquire_slot(ring.fences);
        CHECK_FALSE(none.slot.has_value());
        CHECK(none.wait == ring_wait{ .fence = ring_fence::produced, .value = 2 });

        // Frame 2 complete but not published yet: the acquisition publishes it, and frame 1's slot is free
        ring.fences.produced = 2;
        const slot_acquisition freed = ring.protocol.try_acquire_slot(ring.fences);
        REQUIRE(freed.slot.has_value());
        CHECK(*freed.slot == 0);
        CHECK(ring.protocol.latest_completed() == ring_frame{ .frame = 2, .slot = 1 });
    }

    SECTION("a slot Qt still samples: its pick on consumed")
    {
        ring.frame();
        const qt_pick_result first = ring.pick();
        ring.frame();
        ring.pick();
        ring.frame(); // the third slot, the latest
        const slot_acquisition none = ring.protocol.try_acquire_slot(ring.fences);
        CHECK_FALSE(none.slot.has_value());
        CHECK(none.wait == ring_wait{ .fence = ring_fence::consumed, .value = first.use });

        ring.fences.consumed = first.use;
        const slot_acquisition freed = ring.protocol.try_acquire_slot(ring.fences);
        REQUIRE(freed.slot.has_value());
        CHECK(*freed.slot == first.slot);
    }

    SECTION("a dropped reservation returns the slot")
    {
        const slot_acquisition acquired = ring.protocol.try_acquire_slot(ring.fences);
        REQUIRE(acquired.slot.has_value());
        ring.protocol.release(*acquired.slot);
        const slot_acquisition again = ring.protocol.try_acquire_slot(ring.fences);
        CHECK(again.slot == acquired.slot);
    }
}

TEST_CASE("ring_protocol: publish wakes Qt once per render", "[visualizer][unit]")
{
    ring_protocol protocol;
    CHECK_FALSE(protocol.qt_pick().has_value()); // nothing published: Qt draws its clear colour

    for (const std::uint64_t frame : { 1, 2 })
    {
        const slot_acquisition acquired = protocol.try_acquire_slot({ .produced = frame - 1 });
        REQUIRE(acquired.slot.has_value());
        protocol.bind(*acquired.slot, frame);
        CHECK(protocol.publish(frame, *acquired.slot) == (frame == 1)); // the second finds update_pending set
        CHECK(protocol.update_pending());
    }
    const std::optional<qt_pick_result> pick = protocol.qt_pick();
    REQUIRE(pick.has_value());
    CHECK(pick->frame == 2);
    CHECK_FALSE(protocol.update_pending());

    // No new frame: Qt re-shows frame 2 with a new use, so consumed must reach that one too
    const std::optional<qt_pick_result> again = protocol.qt_pick();
    REQUIRE(again.has_value());
    CHECK(again->frame == 2);
    CHECK(again->slot == pick->slot);
    CHECK(again->use > pick->use);
    CHECK(protocol.picked(pick->slot) == again->use);
}

TEST_CASE("ring_protocol: release after a resize", "[visualizer][unit]")
{
    cpu_ring ring;

    SECTION("a ring Qt never attached to: once its frames are produced, and Qt can't attach any more")
    {
        const slot_acquisition acquired = ring.protocol.try_acquire_slot(ring.fences);
        REQUIRE(acquired.slot.has_value());
        ring.protocol.bind(*acquired.slot, 1);
        ring.protocol.retire();

        const release_check in_flight = ring.protocol.check_released(ring.fences);
        CHECK_FALSE(in_flight.released);
        CHECK(in_flight.wait == ring_wait{ .fence = ring_fence::produced, .value = 1 });

        ring.fences.produced = 1;
        CHECK(ring.protocol.check_released(ring.fences).released);
        CHECK_FALSE(ring.protocol.qt_attach());
    }

    SECTION("a ring Qt attached to: once Qt detached and consumed its detach")
    {
        REQUIRE(ring.protocol.qt_attach());
        ring.frame();
        const qt_pick_result pick = ring.pick();
        ring.fences.consumed = pick.use;
        ring.protocol.retire();

        const release_check attached = ring.protocol.check_released(ring.fences);
        CHECK_FALSE(attached.released);
        CHECK(attached.wait == ring_wait{ .fence = ring_fence::consumed, .value = pick.use + 1 });

        const std::uint64_t detach = ring.protocol.qt_detach();
        CHECK(detach == pick.use + 1);
        CHECK_FALSE(ring.protocol.showing().has_value());
        CHECK_FALSE(ring.protocol.check_released(ring.fences).released);
        ring.fences.consumed = detach;
        CHECK(ring.protocol.check_released(ring.fences).released);
    }
}

TEST_CASE("ring_protocol: a random interleaving never hands Avis a slot Qt still samples", "[visualizer][unit]")
{
    // A reference model of both sides and both GPUs. Avis: acquire, bind (at most two frames in flight, as with
    // frames_in_flight = 2; a reservation can overlap them, as shared_slot_free runs beside begin_frame), its GPU
    // completing frames in order, and the per-frame publish. Qt: render (pick), its GPU completing renders in order,
    // and stretches where Qt doesn't render (minimised).
    struct avis_frame
    {
        std::uint64_t frame;
        std::uint32_t slot;
    };
    struct qt_sample
    {
        std::uint64_t use;
        std::uint32_t slot;
        std::uint64_t frame;
    };

    ring_protocol protocol;
    ring_fence_values fences;
    std::optional<std::uint32_t> reserved;
    std::deque<avis_frame> in_flight; // bound, `produced` not reached
    std::deque<avis_frame> unpublished; // `produced` reached, publish not called yet
    std::deque<qt_sample> sampling; // Qt renders whose `consumed` isn't reached
    std::array<std::uint64_t, ring_slot_count> content{}; // the complete frame in each slot; 0 while written
    std::array<bool, ring_slot_count> writing{}; // reserved or written by Avis
    std::uint64_t next_frame = 0;
    std::uint64_t issued_use = 0;
    bool qt_paused = false;
    int acquisitions = 0;
    int acquisitions_while_paused = 0;
    int picks = 0;
    int produced_waits = 0;
    int consumed_waits = 0;

    std::mt19937 random{ 20261107 };
    for (int step = 0; step < 100'000; ++step)
    {
        if (random() % 400 == 0)
        {
            qt_paused = !qt_paused;
        }
        switch (random() % 6)
        {
        case 0: // Avis: shared_slot_free
            {
                if (reserved)
                {
                    break;
                }
                const slot_acquisition acquired = protocol.try_acquire_slot(fences);
                if (acquired.slot)
                {
                    const std::uint32_t slot = *acquired.slot;
                    for (const qt_sample& sample : sampling)
                    {
                        REQUIRE(sample.slot != slot);
                    }
                    REQUIRE_FALSE(writing[slot]);
                    REQUIRE(protocol.showing() != slot);
                    const std::optional<ring_frame> latest = protocol.latest_completed();
                    REQUIRE((!latest || latest->slot != slot));
                    reserved = slot;
                    writing[slot] = true;
                    ++acquisitions;
                    acquisitions_while_paused += qt_paused ? 1 : 0;
                }
                else if (acquired.wait.fence == ring_fence::produced)
                {
                    // Reached by a frame already bound
                    REQUIRE(acquired.wait.value > fences.produced);
                    REQUIRE_FALSE(in_flight.empty());
                    REQUIRE(acquired.wait.value <= in_flight.back().frame);
                    ++produced_waits;
                }
                else
                {
                    // Reached by a render Qt already submitted, even if Qt renders nothing more
                    REQUIRE(acquired.wait.value > fences.consumed);
                    REQUIRE(acquired.wait.value <= issued_use);
                    ++consumed_waits;
                }
                break;
            }
        case 1: // Avis: the frame is recorded and signals produced, or it is dropped
            if (!reserved || in_flight.size() >= 2)
            {
                break;
            }
            if (random() % 20 == 0)
            {
                protocol.release(*reserved);
                writing[*reserved] = false;
            }
            else
            {
                const std::uint64_t frame = ++next_frame;
                protocol.bind(*reserved, frame);
                content[*reserved] = 0;
                in_flight.push_back({ frame, *reserved });
            }
            reserved.reset();
            break;
        case 2: // Avis's GPU completes a frame
            if (in_flight.empty())
            {
                break;
            }
            fences.produced = in_flight.front().frame;
            content[in_flight.front().slot] = in_flight.front().frame;
            writing[in_flight.front().slot] = false;
            unpublished.push_back(in_flight.front());
            in_flight.pop_front();
            break;
        case 3: // Avis: the per-frame when_reached(produced, n) publishes
            if (unpublished.empty())
            {
                break;
            }
            protocol.publish(unpublished.front().frame, unpublished.front().slot);
            unpublished.pop_front();
            break;
        case 4: // Qt renders
            {
                if (qt_paused)
                {
                    break;
                }
                const std::optional<qt_pick_result> pick = protocol.qt_pick();
                if (!pick)
                {
                    break;
                }
                REQUIRE(pick->frame <= fences.produced);
                REQUIRE(content[pick->slot] == pick->frame);
                REQUIRE_FALSE(writing[pick->slot]);
                sampling.push_back({ pick->use, pick->slot, pick->frame });
                issued_use = pick->use;
                ++picks;
                break;
            }
        case 5: // Qt's GPU finishes a render: still the frame it picked, then consumed
            if (sampling.empty())
            {
                break;
            }
            REQUIRE(content[sampling.front().slot] == sampling.front().frame);
            REQUIRE_FALSE(writing[sampling.front().slot]);
            fences.consumed = sampling.front().use;
            sampling.pop_front();
            break;
        default:
            break;
        }
    }

    CHECK(acquisitions > 3'000);
    CHECK(acquisitions_while_paused > 300); // Qt not rendering doesn't stall Avis
    CHECK(picks > 5'000);
    CHECK(produced_waits > 0);
    CHECK(consumed_waits > 0);
}

namespace
{
    // ---- GPU ----

    // The Visualizer's QWinEventNotifier, as a test loop: waits on the completion event and polls until `done`
    void drive_until(rhi::completion_driver driver, const std::function<bool()>& done)
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

    template<class Value>
    struct outcome
    {
        std::optional<Value> value;
        std::optional<avis::error> error;
        bool stopped = false;

        bool done() const noexcept
        {
            return value.has_value() || error.has_value() || stopped;
        }
    };

    template<class Value>
    struct outcome_receiver
    {
        using receiver_concept = stdexec::receiver_tag;

        template<class... Values>
        void set_value(Values&&... values) noexcept
        {
            result->value.emplace(std::forward<Values>(values)...);
        }

        void set_error(avis::error error) noexcept
        {
            result->error.emplace(std::move(error));
        }

        void set_stopped() noexcept
        {
            result->stopped = true;
        }

        auto get_env() const noexcept
        {
            return stdexec::prop{ stdexec::get_stop_token, token };
        }

        outcome<Value>* result;
        stdexec::inplace_stop_token token;
    };

    // A started sender with its outcome and stop source; not movable
    template<class Sender, class Value>
    struct started
    {
        explicit started(Sender sender) :
            operation{ stdexec::connect(std::move(sender), outcome_receiver<Value>{ &result, source.get_token() }) }
        {
            stdexec::start(operation);
        }

        outcome<Value> result;
        stdexec::inplace_stop_source source;
        stdexec::connect_result_t<Sender, outcome_receiver<Value>> operation;
    };

    using started_slot = started<shared_slot_sender, ring_slot>;

    std::uint32_t colour_of(const std::uint64_t frame)
    {
        const auto byte = [](const std::uint64_t value) { return static_cast<std::uint32_t>(value & 0xFF); };
        return byte(frame * 40) | (byte(255 - frame) << 8) | (byte(frame * 7 + 3) << 16) | 0xFF00'0000u;
    }

    // The Avis side of a frame: an upload copied into the slot, from COMMON back to COMMON, then signal_produced; the
    // renderer's present_to_shared does the same with ldr_color (11 §12)
    class avis_writer
    {
    public:
        explicit avis_writer(avis::tests::test_rhi& hardware_interface) :
            hardware_interface_{ hardware_interface },
            resources_{ *hardware_interface },
            queue_{ *hardware_interface },
            driver_{ *hardware_interface }
        {}

        ~avis_writer()
        {
            hardware_interface_->wait_idle();
            for (const rhi::buffer_handle buffer : uploads_)
            {
                resources_.destroy(buffer);
            }
        }

        avis_writer(const avis_writer&) = delete;
        avis_writer& operator=(const avis_writer&) = delete;

        void write(shared_frame_ring& ring, ring_slot slot, const std::uint64_t frame)
        {
            const rhi::texture_handle texture = ring.texture(slot.index());
            const rhi::copy_footprint footprint = resources_.copyable_footprint(texture, {});
            auto upload = resources_.create_buffer(
                { .size = footprint.total_size,
                  .usage = rhi::buffer_usage::copy_source,
                  .memory = rhi::memory_type::upload,
                  .debug_name = "ring test upload" });
            REQUIRE(upload.has_value());
            uploads_.push_back(*upload);
            const std::span<std::byte> bytes = resources_.mapped_bytes(*upload);
            const std::uint32_t colour = colour_of(frame);
            for (std::uint32_t row = 0; row < footprint.height; ++row)
            {
                for (std::uint32_t column = 0; column < footprint.width; ++column)
                {
                    std::memcpy(bytes.data() + footprint.offset + row * footprint.row_pitch + column * 4, &colour, 4);
                }
            }

            auto list =
                rhi::command_lists{ *hardware_interface_ }.acquire<rhi::queue_type::graphics>("ring test frame");
            REQUIRE(list.has_value());
            const std::array to_copy{ rhi::texture_barrier{ .texture = texture,
                                                            .before = rhi::access_states::common,
                                                            .after = rhi::access_states::copy_destination } };
            list->barrier({}, {}, to_copy);
            list->copy_buffer_to_texture(texture, {}, *upload, 0);
            const std::array to_common{ rhi::texture_barrier{ .texture = texture,
                                                              .before = rhi::access_states::copy_destination,
                                                              .after = rhi::access_states::common } };
            list->barrier({}, {}, to_common);
            REQUIRE(queue_.submit(std::span{ &*list, 1 }).has_value());
            ring.signal_produced(queue_, std::move(slot), frame);
        }

        // A whole frame as the frame chain runs it: the gate (inline: a slot must be free), the write, and the
        // publish once produced is reached. Returns the slot.
        std::uint32_t frame(shared_frame_ring& ring, const std::uint64_t frame)
        {
            started_slot gate{ ring.shared_slot_free() };
            REQUIRE(gate.result.value.has_value());
            const std::uint32_t slot = gate.result.value->index();
            write(ring, std::move(*gate.result.value), frame);
            publish(ring, frame, slot);
            return slot;
        }

        void publish(shared_frame_ring& ring, const std::uint64_t frame, const std::uint32_t slot)
        {
            started<rhi::gpu_wait_sender, std::monostate> produced{ ring.when_produced(frame) };
            drive_until(driver_, [&] { return produced.result.done(); });
            REQUIRE(produced.result.value.has_value());
            ring.publish(frame, slot);
        }

    private:
        avis::tests::test_rhi& hardware_interface_;
        rhi::resources resources_;
        rhi::queue<rhi::queue_type::graphics> queue_;
        rhi::completion_driver driver_;
        std::vector<rhi::buffer_handle> uploads_;
    };

    // Qt's side, natively: a device on the RHI's adapter with its own direct queue, command list and fence. In one
    // process that device is the RHI's ID3D12Device object (12 §3.1), as it is for Qt; what stands in for Qt is the
    // separate queue and fences, and opening every handle with OpenSharedHandle.
    class fake_qt
    {
    public:
        // A ring as Qt imports it
        struct imported
        {
            std::shared_ptr<shared_ring_state> state;
            std::array<rhi::com_ptr<ID3D12Resource>, ring_slot_count> textures;
            rhi::com_ptr<ID3D12Fence> produced;
            rhi::com_ptr<ID3D12Fence> consumed;
        };

        explicit fake_qt(rhi::render_hardware_interface& hardware_interface)
        {
            REQUIRE(SUCCEEDED(D3D12CreateDevice(
                &hardware_interface.state().device().adapter(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_))));
            const D3D12_COMMAND_QUEUE_DESC description{ .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
            REQUIRE(SUCCEEDED(device_->CreateCommandQueue(&description, IID_PPV_ARGS(&queue_))));
            REQUIRE(
                SUCCEEDED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))));
            REQUIRE(SUCCEEDED(device_->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.get(), nullptr, IID_PPV_ARGS(&list_))));
            REQUIRE(SUCCEEDED(list_->Close()));
            REQUIRE(SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done_))));
        }

        ~fake_qt()
        {
            wait_idle();
        }

        fake_qt(const fake_qt&) = delete;
        fake_qt& operator=(const fake_qt&) = delete;

        // After qt_attach, as viewport_renderer does on a new generation
        imported import(std::shared_ptr<shared_ring_state> state)
        {
            imported ring{ .state = std::move(state) };
            for (std::uint32_t slot = 0; slot < ring_slot_count; ++slot)
            {
                REQUIRE(SUCCEEDED(device_->OpenSharedHandle(
                    static_cast<HANDLE>(ring.state->textures[slot].value), IID_PPV_ARGS(&ring.textures[slot]))));
            }
            REQUIRE(SUCCEEDED(device_->OpenSharedHandle(
                static_cast<HANDLE>(ring.state->produced.value), IID_PPV_ARGS(&ring.produced))));
            REQUIRE(SUCCEEDED(device_->OpenSharedHandle(
                static_cast<HANDLE>(ring.state->consumed.value), IID_PPV_ARGS(&ring.consumed))));
            return ring;
        }

        // Qt's render() for `pick`: a GPU wait on produced, a copy of the slot into a readback buffer (where Qt draws
        // its fullscreen triangle), and, unless held back, the signal of consumed with the pick's use. Returns the
        // slot's texels.
        std::vector<std::uint32_t> sample(imported& ring, const qt_pick_result& pick, const bool signal_consumed = true)
        {
            ID3D12Resource& texture = *ring.textures[pick.slot].get();
            const D3D12_RESOURCE_DESC description = texture.GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
            UINT64 total = 0;
            device_->GetCopyableFootprints(&description, 0, 1, 0, &layout, nullptr, nullptr, &total);

            const D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_READBACK };
            const D3D12_RESOURCE_DESC buffer{ .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                              .Width = total,
                                              .Height = 1,
                                              .DepthOrArraySize = 1,
                                              .MipLevels = 1,
                                              .SampleDesc = { .Count = 1 },
                                              .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
            rhi::com_ptr<ID3D12Resource> readback;
            REQUIRE(SUCCEEDED(device_->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&readback))));

            wait_idle();
            REQUIRE(SUCCEEDED(allocator_->Reset()));
            REQUIRE(SUCCEEDED(list_->Reset(allocator_.get(), nullptr)));
            // The texture is in COMMON: the copy promotes it to COPY_SOURCE, and it decays back to COMMON at the end
            const D3D12_TEXTURE_COPY_LOCATION source{ .pResource = &texture,
                                                      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                      .SubresourceIndex = 0 };
            const D3D12_TEXTURE_COPY_LOCATION destination{ .pResource = readback.get(),
                                                           .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                           .PlacedFootprint = layout };
            list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            REQUIRE(SUCCEEDED(list_->Close()));

            REQUIRE(SUCCEEDED(queue_->Wait(ring.produced.get(), pick.frame)));
            ID3D12CommandList* const lists[] = { list_.get() };
            queue_->ExecuteCommandLists(1, lists);
            if (signal_consumed)
            {
                REQUIRE(SUCCEEDED(queue_->Signal(ring.consumed.get(), pick.use)));
            }
            wait_idle();

            void* mapped = nullptr;
            REQUIRE(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
            std::vector<std::uint32_t> texels;
            texels.reserve(std::size_t{ layout.Footprint.Width } * layout.Footprint.Height);
            for (UINT row = 0; row < layout.Footprint.Height; ++row)
            {
                const auto* const first = static_cast<const std::byte*>(mapped) + layout.Offset +
                                          std::size_t{ row } * layout.Footprint.RowPitch;
                for (UINT column = 0; column < layout.Footprint.Width; ++column)
                {
                    std::uint32_t texel = 0;
                    std::memcpy(&texel, first + std::size_t{ column } * 4, 4);
                    texels.push_back(texel);
                }
            }
            readback->Unmap(0, nullptr);
            return texels;
        }

        void signal_consumed(const imported& ring, const std::uint64_t value)
        {
            REQUIRE(SUCCEEDED(queue_->Signal(ring.consumed.get(), value)));
        }

        void wait_idle()
        {
            const std::uint64_t value = ++done_value_;
            REQUIRE(SUCCEEDED(queue_->Signal(done_.get(), value)));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 10 };
            while (done_->GetCompletedValue() < value)
            {
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                SwitchToThread();
            }
        }

    private:
        rhi::com_ptr<ID3D12Device> device_;
        rhi::com_ptr<ID3D12CommandQueue> queue_;
        rhi::com_ptr<ID3D12CommandAllocator> allocator_;
        rhi::com_ptr<ID3D12GraphicsCommandList> list_;
        rhi::com_ptr<ID3D12Fence> done_;
        std::uint64_t done_value_ = 0;
    };

    // The rings' setup: the RHI fixture, the ring set with a first ring, and Qt attached to it
    struct ring_fixture
    {
        ring_fixture() : rings{ rhi::sharing{ *hardware_interface }, rhi::resources{ *hardware_interface } }
        {
            rings.request_resize({ .width = 8, .height = 4 });
            auto current = rings.update();
            REQUIRE(current.has_value());
            REQUIRE(*current != nullptr);
            ring = *current;
        }

        ~ring_fixture()
        {
            qt.wait_idle();
            hardware_interface->wait_idle();
        }

        fake_qt::imported attach(const std::shared_ptr<shared_ring_state>& state)
        {
            REQUIRE(state->protocol.qt_attach());
            return qt.import(state);
        }

        avis::tests::test_rhi hardware_interface;
        rhi::completion_driver driver{ *hardware_interface };
        avis_writer writer{ hardware_interface };
        shared_frame_rings rings;
        shared_frame_ring* ring = nullptr;
        fake_qt qt{ *hardware_interface };
    };
} // namespace

TEST_CASE_METHOD(ring_fixture, "shared frame ring: Qt's device reads the frames Avis wrote", "[visualizer]")
{
    CHECK(ring->generation() == 1);
    CHECK(ring->size() == avis::graphics::texture_extent{ .width = 8, .height = 4 });
    REQUIRE(rings.qt_current() == ring->state());
    fake_qt::imported imported = attach(rings.qt_current());

    for (std::uint64_t frame = 1; frame <= 6; ++frame)
    {
        const std::uint32_t slot = writer.frame(*ring, frame);
        const std::optional<qt_pick_result> pick = imported.state->protocol.qt_pick();
        REQUIRE(pick.has_value());
        CHECK(pick->frame == frame);
        CHECK(pick->slot == slot);

        const std::vector<std::uint32_t> texels = qt.sample(imported, *pick);
        REQUIRE(texels.size() == 8 * 4);
        for (const std::uint32_t texel : texels)
        {
            REQUIRE(texel == colour_of(frame));
        }
    }
    CHECK(ring->fence_values().consumed == imported.state->protocol.picked(*imported.state->protocol.showing()));
}

TEST_CASE_METHOD(
    ring_fixture, "shared frame ring: shared_slot_free completes once Qt's device signals consumed", "[visualizer]")
{
    fake_qt::imported imported = attach(rings.qt_current());

    // Qt samples frames 1 and 2 without signalling consumed: it holds two slots. Frame 3 takes the third.
    writer.frame(*ring, 1);
    const qt_pick_result first = *imported.state->protocol.qt_pick();
    qt.sample(imported, first, false);
    writer.frame(*ring, 2);
    const qt_pick_result second = *imported.state->protocol.qt_pick();
    qt.sample(imported, second, false);
    writer.frame(*ring, 3);

    SECTION("it waits, then gets the slot Qt released")
    {
        started_slot gate{ ring->shared_slot_free() };
        CHECK_FALSE(gate.result.done());
        driver.poll_completions(); // nothing on the GPU completes it
        CHECK_FALSE(gate.result.done());

        qt.signal_consumed(imported, first.use);
        drive_until(driver, [&] { return gate.result.done(); });
        REQUIRE(gate.result.value.has_value());
        CHECK(gate.result.value->index() == first.slot);
    }

    SECTION("a stop request ends the wait")
    {
        started_slot gate{ ring->shared_slot_free() };
        CHECK(driver.has_pending_work());
        gate.source.request_stop();
        CHECK(gate.result.stopped);
        CHECK_FALSE(driver.has_pending_work());

        // Nothing was reserved: the next gate gets the slot once Qt is done
        qt.signal_consumed(imported, second.use);
        started_slot next{ ring->shared_slot_free() };
        drive_until(driver, [&] { return next.result.done(); });
        REQUIRE(next.result.value.has_value());
    }
}

TEST_CASE_METHOD(ring_fixture, "shared frame ring: a resize keeps the old ring until Qt released it", "[visualizer]")
{
    fake_qt::imported imported = attach(rings.qt_current());
    writer.frame(*ring, 1);
    qt.sample(imported, *imported.state->protocol.qt_pick());

    // The same size makes no new ring; a zero side is ignored
    rings.request_resize({ .width = 8, .height = 4 });
    CHECK(*rings.update() == ring);
    rings.request_resize({ .width = 0, .height = 4 });
    CHECK(*rings.update() == ring);

    rings.request_resize({ .width = 16, .height = 8 });
    auto resized = rings.update();
    REQUIRE(resized.has_value());
    shared_frame_ring* const second = *resized;
    REQUIRE(second != ring);
    CHECK(second->generation() == 2);
    CHECK(second->size() == avis::graphics::texture_extent{ .width = 16, .height = 8 });
    CHECK(rings.qt_current() == second->state());
    REQUIRE(rings.retired().size() == 1);
    CHECK(rings.retired().front().get() == ring);

    // Qt is still attached to the first ring
    CHECK(rings.destroy_released() == 1);
    started<ring_release_sender, std::monostate> released{ ring->when_released() };
    driver.poll_completions();
    CHECK_FALSE(released.result.done());

    // Qt sees generation 2: it detaches from the first ring, signals the detach after its render, and lets go
    const std::uint64_t detach = imported.state->protocol.qt_detach();
    qt.signal_consumed(imported, detach);
    qt.wait_idle();
    imported = {};
    drive_until(driver, [&] { return released.result.done(); });
    REQUIRE(released.result.value.has_value());
    CHECK(rings.destroy_released() == 0);
    ring = second;

    // Qt reads the new ring at the new size
    fake_qt::imported resized_import = attach(rings.qt_current());
    writer.frame(*second, 2);
    const std::vector<std::uint32_t> texels = qt.sample(resized_import, *resized_import.state->protocol.qt_pick());
    REQUIRE(texels.size() == 16 * 8);
    CHECK(texels.front() == colour_of(2));
    CHECK(texels.back() == colour_of(2));

    // A ring Qt never attached to is released at once, and Qt can't attach to it any more
    rings.request_resize({ .width = 4, .height = 4 });
    REQUIRE(rings.update().has_value());
    const std::shared_ptr<shared_ring_state> skipped = rings.qt_current();
    rings.request_resize({ .width = 8, .height = 8 });
    REQUIRE(rings.update().has_value());
    CHECK(rings.qt_current()->generation == 4);
    CHECK_FALSE(skipped->protocol.qt_attach());

    // Generation 2, which Qt still shows, stays; generation 3 goes
    CHECK(rings.destroy_released() == 1);
    qt.signal_consumed(resized_import, resized_import.state->protocol.qt_detach());
    qt.wait_idle();
    resized_import = {};
    started<ring_release_sender, std::monostate> second_released{ rings.retired().front()->when_released() };
    drive_until(driver, [&] { return second_released.result.done(); });
    CHECK(rings.destroy_released() == 0);
}
