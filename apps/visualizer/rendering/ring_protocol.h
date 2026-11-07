#ifndef AVIS_VISUALIZER_RENDERING_RING_PROTOCOL_H
#define AVIS_VISUALIZER_RENDERING_RING_PROTOCOL_H

// The protocol of the shared frame ring (docs/design/12-visualizer.md §3.3), without its GPU objects, so that it can be
// tested on the CPU. The fences' values come in as ring_fence_values; shared_frame_ring reads them from the shared
// fences.
//
// Two sides use it:
// - Avis (the frame chain, 12 §3.4): try_acquire_slot, bind, release, publish, retire, check_released. These calls are
//   serialised by a mutex that only Avis takes.
// - Qt (the render thread, 12.5): qt_attach, qt_pick, qt_detach. They never block: Qt only writes atomics.
//
// Two additions to the rule of 12 §3.3:
// - **Avis's own slots.** A slot is also busy while Avis writes into it (reserved or bound to a frame that isn't
//   published yet) and while it holds the latest published frame, which Qt may pick at any time. With two frames in
//   flight that can be all three slots for a moment; the wait then is for Avis's own oldest frame on `produced`, after
//   which that frame is published here and the slot it replaces may be free.
// - **Qt's uses, not frame numbers.** `picked[i]` and the values Qt signals on `consumed` count Qt's own renders that
//   sampled a slot (`use`), not Avis frame numbers. When nothing new is published, Qt re-shows the same frame in a new
//   render; with frame numbers, `consumed` would already be at that frame from the first render, and the slot would
//   look free while Qt still samples it.

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace avis::visualizer::rendering
{
    inline constexpr std::uint32_t ring_slot_count = 3;

    // The completed values of the ring's shared fences, read before a decision
    struct ring_fence_values
    {
        std::uint64_t produced = 0; // signalled by Avis: frame n is in its slot
        std::uint64_t consumed = 0; // signalled by Qt: its use m is done sampling
    };

    enum class ring_fence : std::uint8_t
    {
        produced,
        consumed
    };

    // A value one of the fences hasn't reached yet, after which the decision may change
    struct ring_wait
    {
        ring_fence fence = ring_fence::consumed;
        std::uint64_t value = 0;

        friend bool operator==(const ring_wait&, const ring_wait&) noexcept = default;
    };

    // latest_completed (12 §3.3)
    struct ring_frame
    {
        std::uint64_t frame = 0;
        std::uint32_t slot = 0;

        friend bool operator==(const ring_frame&, const ring_frame&) noexcept = default;
    };

    // What Qt's render() samples: frame `frame` in slot `slot`. Qt waits on `produced` for `frame` (already reached),
    // and signals `consumed` with `use` after the render's submission (12 §3.3 rule 4).
    struct qt_pick_result
    {
        std::uint64_t frame = 0;
        std::uint32_t slot = 0;
        std::uint64_t use = 0;
    };

    struct slot_acquisition
    {
        std::optional<std::uint32_t> slot; // reserved for the caller
        ring_wait wait; // without a slot: what to wait for before trying again
    };

    struct release_check
    {
        bool released = false;
        ring_wait wait; // when not released
    };

    class ring_protocol
    {
    public:
        ring_protocol() = default;
        ring_protocol(const ring_protocol&) = delete;
        ring_protocol& operator=(const ring_protocol&) = delete;

        // ---- Avis ----

        // First publishes the newest bound frame that `values.produced` shows complete. Then reserves a free slot: not
        // Qt's `showing`, not Avis's own (reserved, in flight or the latest), and either never picked or with
        // `consumed` at its last pick. Prefers the slot holding the oldest frame: Qt always picks the newest, so that
        // is the one Qt released longest ago. Without a free slot, returns the value to wait for, which `values` hasn't
        // reached. One reservation at a time (debug check).
        slot_acquisition try_acquire_slot(ring_fence_values values);

        // The reserved slot will hold `frame`, which Avis signals on `produced` once written. Frames increase.
        void bind(std::uint32_t slot, std::uint64_t frame);

        // The reservation isn't used: the frame was stopped or failed before signalling `produced`
        void release(std::uint32_t slot);

        // When `produced` reached `frame`: latest_completed = { frame, slot } unless a newer frame was published.
        // Returns true when the caller must wake Qt (post update()): update_pending was clear.
        bool publish(std::uint64_t frame, std::uint32_t slot);

        std::optional<ring_frame> latest_completed() const noexcept;

        // The ring isn't current any more (a resize, 12 §3.3 rule 8). A ring Qt never attached to can then never be
        // attached; one it attached to is released once Qt detaches and `consumed` reaches the detach.
        void retire();

        // Retired, with nothing of Avis's in flight on `produced`, and Qt either never attached or detached and done
        // sampling. Without that, the value to wait for.
        release_check check_released(ring_fence_values values) const;

        // ---- Qt (render thread) ----

        // Before importing the ring's handles. False when the ring was retired first: Qt then takes the newer ring.
        bool qt_attach() noexcept;

        // In render(): clears update_pending, then picks latest_completed and marks it `showing` and picked with a new
        // use. Empty before Avis published anything (Qt draws its clear colour).
        std::optional<qt_pick_result> qt_pick() noexcept;

        // When Qt moves to a newer ring: no slot is showing any more. Returns a last use, which Qt signals on
        // `consumed` after this render's submission, as it does for a pick.
        std::uint64_t qt_detach() noexcept;

        // ---- Observers, for tests and logs ----
        std::optional<std::uint32_t> showing() const noexcept;
        std::uint64_t picked(std::uint32_t slot) const noexcept;
        bool update_pending() const noexcept;

    private:
        enum class qt_state : std::uint8_t
        {
            fresh, // Qt hasn't attached
            attached,
            detached,
            retired_unattached // retired before Qt attached: Qt never will
        };

        static constexpr std::uint64_t pack(std::uint64_t frame, std::uint32_t slot) noexcept
        {
            return (frame << 2) | slot;
        }

        void catch_up(std::uint64_t produced); // under avis_mutex_

        // Shared with Qt; each written by one side only (latest_ by Avis, picked_/showing_ by Qt, update_pending_ set
        // by Avis and cleared by Qt, state_ by compare-exchange)
        std::atomic<std::uint64_t> latest_{ 0 }; // pack(frame, slot); 0: nothing published
        std::array<std::atomic<std::uint64_t>, ring_slot_count> picked_{}; // Qt's last use of each slot; 0: never
        std::atomic<std::int32_t> showing_{ -1 };
        std::atomic<bool> update_pending_{ false };
        std::atomic<qt_state> state_{ qt_state::fresh };
        std::atomic<std::uint64_t> final_use_{ 0 }; // qt_detach's use

        // Qt's render thread only
        std::uint64_t qt_uses_ = 0;

        // Avis only
        mutable std::mutex avis_mutex_;
        std::array<std::uint64_t, ring_slot_count> frames_{}; // the frame bound to each slot; 0: never
        std::optional<std::uint32_t> reserved_;
        bool retired_ = false;
    };
} // namespace avis::visualizer::rendering

#endif
