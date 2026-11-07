#ifndef AVIS_VISUALIZER_RENDERING_SHARED_FRAME_RING_H
#define AVIS_VISUALIZER_RENDERING_SHARED_FRAME_RING_H

// The shared frame ring between the Avis renderer and Qt (docs/design/12-visualizer.md §3.3, §3.4, 09 §13): three
// shared RGBA8_UNORM textures at the viewport's pixel size and the shared fences `produced` and `consumed`, all created
// by Avis and exported as rhi::shared_handle, with the CPU protocol of ring_protocol. shared_frame_rings keeps the
// current ring and the rings a resize retired. Free of Qt and D3D12: the Qt side opens the handles in
// ui/viewport_renderer (12.5).

#include "rendering/ring_protocol.h"
#include "rendering/view.h"

#include "avis/error.h"
#include "avis/graphics/texture.h"
#include "avis/rhi/detail/gpu_wait_sender.h"
#include "avis/rhi/queue.h"
#include "avis/rhi/resources.h"
#include "avis/rhi/shared.h"
#include "avis/rhi/types.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace avis::visualizer::rendering
{
    class shared_frame_ring;

    namespace detail
    {
        struct slot_policy;
    } // namespace detail

    // What the Qt render thread uses of a ring. Qt keeps it alive with its shared_ptr while it uses the ring; the
    // handles stay valid while Qt is attached (ring_protocol::qt_attach), because a ring isn't destroyed before Qt
    // detaches from it.
    struct shared_ring_state
    {
        graphics::texture_extent size;
        std::uint64_t generation = 0;
        std::array<rhi::shared_handle, ring_slot_count> textures{};
        rhi::shared_handle produced;
        rhi::shared_handle consumed;
        ring_protocol protocol;
    };

    // A slot reserved by shared_slot_free() for one frame. Move-only. Dropped without signal_produced, it goes back to
    // the ring (a frame stopped or failed before anything was written for Qt).
    class ring_slot
    {
    public:
        ring_slot(ring_slot&& other) noexcept :
            protocol_{ std::exchange(other.protocol_, nullptr) },
            index_{ other.index_ }
        {}

        ring_slot& operator=(ring_slot&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                protocol_ = std::exchange(other.protocol_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }

        ring_slot(const ring_slot&) = delete;
        ring_slot& operator=(const ring_slot&) = delete;

        ~ring_slot()
        {
            reset();
        }

        std::uint32_t index() const noexcept
        {
            return index_;
        }

    private:
        friend class shared_frame_ring;
        friend struct detail::slot_policy;

        ring_slot(ring_protocol& protocol, const std::uint32_t index) noexcept : protocol_{ &protocol }, index_{ index }
        {}

        void reset() noexcept
        {
            if (protocol_ != nullptr)
            {
                std::exchange(protocol_, nullptr)->release(index_);
            }
        }

        ring_protocol* protocol_;
        std::uint32_t index_;
    };

    namespace detail
    {
        // A ring decision that may need GPU progress: the Policy's attempt() returns its value, or a gpu_wait_sender
        // for a fence value not reached yet; the operation then waits and tries again. The waits complete from the
        // RHI's completion driver (12 §3.4), so nothing blocks, and nothing waits on the GPU.
        //
        // Policy: `using value_type` (std::monostate for none) and `std::variant<value_type, rhi::gpu_wait_sender>
        // attempt() noexcept`.
        template<class Policy, class Receiver>
        class ring_wait_operation_state
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            ring_wait_operation_state(Receiver receiver, Policy policy) noexcept(
                std::is_nothrow_move_constructible_v<Receiver>) :
                receiver_{ std::move(receiver) },
                policy_{ std::move(policy) }
            {}

            ring_wait_operation_state(const ring_wait_operation_state&) = delete;
            ring_wait_operation_state& operator=(const ring_wait_operation_state&) = delete;

            void start() & noexcept
            {
                run(no_wait);
            }

        private:
            static constexpr int no_wait = -1;

            enum class outcome : std::uint8_t
            {
                reached,
                failed,
                stopped
            };

            // Receives one wait; lends it the outer receiver's environment, so a stop request stops the wait
            struct wait_receiver
            {
                using receiver_concept = stdexec::receiver_tag;

                void set_value() noexcept
                {
                    self->on_wait_done(index, outcome::reached);
                }

                void set_error(avis::error error) noexcept
                {
                    self->error_ = std::move(error);
                    self->on_wait_done(index, outcome::failed);
                }

                void set_stopped() noexcept
                {
                    self->on_wait_done(index, outcome::stopped);
                }

                stdexec::env_of_t<Receiver> get_env() const noexcept
                {
                    return stdexec::get_env(self->receiver_);
                }

                ring_wait_operation_state* self;
                int index;
            };

            using wait_operation = stdexec::connect_result_t<rhi::gpu_wait_sender, wait_receiver>;

            struct wait_connector
            {
                ring_wait_operation_state* self;
                int index;
                rhi::gpu_wait_sender sender;

                operator wait_operation() && noexcept
                {
                    return stdexec::connect(std::move(sender), wait_receiver{ self, index });
                }
            };

            // Tries until a value or an asynchronous wait. `busy` is the wait whose completion called this, still on
            // the stack, so the other one is used. A wait that completed inside start() has returned, so its storage
            // is reused at once.
            void run(const int busy) noexcept
            {
                const int index = busy == 0 ? 1 : 0;
                for (;;)
                {
                    auto attempt = policy_.attempt();
                    if (attempt.index() == 0)
                    {
                        deliver(std::move(std::get<0>(attempt)));
                        return;
                    }

                    waits_[index].reset();
                    phase_.store(starting);
                    waits_[index].emplace(wait_connector{ this, index, std::move(std::get<1>(attempt)) });
                    stdexec::start(*waits_[index]);
                    int expected = starting;
                    if (phase_.compare_exchange_strong(expected, waiting))
                    {
                        return; // completes later, from on_wait_done: touch nothing more
                    }
                    // Completed inside start()
                    if (outcome_ != outcome::reached)
                    {
                        finish();
                        return;
                    }
                }
            }

            void on_wait_done(const int index, const outcome result) noexcept
            {
                outcome_ = result;
                int expected = starting;
                if (phase_.compare_exchange_strong(expected, completed_inline))
                {
                    return; // run() goes on after start() returns
                }
                if (result == outcome::reached)
                {
                    run(index);
                }
                else
                {
                    finish();
                }
            }

            void finish() noexcept
            {
                if (outcome_ == outcome::failed)
                {
                    stdexec::set_error(std::move(receiver_), std::move(error_));
                }
                else
                {
                    stdexec::set_stopped(std::move(receiver_));
                }
            }

            template<class Value>
            void deliver(Value&& value) noexcept
            {
                if constexpr (std::is_same_v<std::remove_cvref_t<Value>, std::monostate>)
                {
                    stdexec::set_value(std::move(receiver_));
                }
                else
                {
                    stdexec::set_value(std::move(receiver_), std::forward<Value>(value));
                }
            }

            static constexpr int starting = 0;
            static constexpr int waiting = 1;
            static constexpr int completed_inline = 2;

            Receiver receiver_;
            Policy policy_;
            std::array<std::optional<wait_operation>, 2> waits_;
            std::atomic<int> phase_{ starting };
            outcome outcome_ = outcome::reached;
            avis::error error_;
        };

        template<class Value>
        struct ring_wait_completions
        {
            using type = avis::completions<Value>;
        };

        template<>
        struct ring_wait_completions<std::monostate>
        {
            using type = avis::completions<>;
        };

        template<class Policy>
        class ring_wait_sender
        {
        public:
            using sender_concept = stdexec::sender_tag;
            using completion_signatures = typename ring_wait_completions<typename Policy::value_type>::type;

            explicit ring_wait_sender(Policy policy) noexcept : policy_{ std::move(policy) } {}

            template<stdexec::receiver Receiver>
            auto connect(Receiver receiver) && noexcept(std::is_nothrow_move_constructible_v<Receiver>)
                -> ring_wait_operation_state<Policy, Receiver>
            {
                return { std::move(receiver), std::move(policy_) };
            }

        private:
            Policy policy_;
        };

        struct slot_policy
        {
            using value_type = ring_slot;
            shared_frame_ring* ring;
            std::variant<ring_slot, rhi::gpu_wait_sender> attempt() noexcept;
        };

        struct release_policy
        {
            using value_type = std::monostate;
            shared_frame_ring* ring;
            std::variant<std::monostate, rhi::gpu_wait_sender> attempt() noexcept;
        };
    } // namespace detail

    // Completes with a reserved ring_slot (12 §3.4's CPU-side gate): inline when a slot is free, the normal case;
    // otherwise once `produced` or `consumed` reaches the value ring_protocol names, through
    // rhi::sharing::when_reached, and tries again. Never waits on the GPU. set_error with device_lost, set_stopped when
    // the receiver's stop token fires while it waits. Single-shot; never outlives its ring.
    using shared_slot_sender = detail::ring_wait_sender<detail::slot_policy>;

    // Completes with set_value() once a retired ring is released (ring_protocol::check_released), the same way
    using ring_release_sender = detail::ring_wait_sender<detail::release_policy>;

    static_assert(avis::sender_of<shared_slot_sender, ring_slot>);
    static_assert(avis::sender_of<ring_release_sender>);

    class shared_frame_ring
    {
    public:
        // Three shared textures of `size` (shader_resource for Qt, copy_destination for present_to_shared, COMMON
        // initially) and the two fences, all exported. Fails when size has a zero side.
        static result<std::unique_ptr<shared_frame_ring>> create(
            rhi::sharing sharing, rhi::resources resources, graphics::texture_extent size, std::uint64_t generation);

        // Destroys the textures and fences through the RHI's deferred release. Only once released(), or after the GPU
        // and Qt are done with the ring (shutdown), and with no sender of this ring pending.
        ~shared_frame_ring();

        shared_frame_ring(const shared_frame_ring&) = delete;
        shared_frame_ring& operator=(const shared_frame_ring&) = delete;

        graphics::texture_extent size() const noexcept
        {
            return state_->size;
        }

        std::uint64_t generation() const noexcept
        {
            return state_->generation;
        }

        // For the Qt render thread (12.5)
        const std::shared_ptr<shared_ring_state>& state() const noexcept
        {
            return state_;
        }

        ring_protocol& protocol() noexcept
        {
            return state_->protocol;
        }

        rhi::texture_handle texture(std::uint32_t slot) const noexcept
        {
            return textures_[slot];
        }

        // The view output for a reserved slot (present_to_shared, 11 §12): COMMON before and after, as 12 §3.3 asks
        output_target output(const ring_slot& slot) const noexcept
        {
            return { .texture = textures_[slot.index()] };
        }

        ring_fence_values fence_values() const noexcept;

        // The gate of 12 §3.4. One at a time: the frame chain binds or drops the slot before asking again.
        shared_slot_sender shared_slot_free() noexcept
        {
            return shared_slot_sender{ detail::slot_policy{ this } };
        }

        // After the frame's submissions that write the slot: binds it to `frame` and signals `produced` with `frame`
        // on `queue`
        void signal_produced(rhi::queue<rhi::queue_type::graphics>& queue, ring_slot slot, std::uint64_t frame);

        // When frame `frame` is in its slot on the GPU; then publish() it
        rhi::gpu_wait_sender when_produced(std::uint64_t frame);

        // latest_completed = { frame, slot }; true when the caller posts the item's update() (12 §3.3 rule 5)
        bool publish(std::uint64_t frame, std::uint32_t slot);

        // A resize made a newer ring current: from now on Qt can't attach to this one (shared_frame_rings::update)
        void retire();

        bool released() const;

        // Completes once released(); the frame chain spawns it, then destroys the ring (12 §3.3 rule 8)
        ring_release_sender when_released() noexcept
        {
            return ring_release_sender{ detail::release_policy{ this } };
        }

    private:
        friend struct detail::slot_policy;
        friend struct detail::release_policy;

        shared_frame_ring(rhi::sharing sharing, rhi::resources resources) noexcept;

        rhi::gpu_wait_sender wait_for(ring_wait wait);

        rhi::sharing sharing_;
        rhi::resources resources_;
        std::array<rhi::texture_handle, ring_slot_count> textures_{};
        std::optional<rhi::shared_fence> produced_;
        std::optional<rhi::shared_fence> consumed_;
        std::shared_ptr<shared_ring_state> state_;
    };

    // The current ring and the retired ones (12 §3.3 rule 8). The frame chain owns it; request_resize comes from the
    // GUI thread and qt_current from Qt's render thread.
    class shared_frame_rings
    {
    public:
        shared_frame_rings(rhi::sharing sharing, rhi::resources resources) noexcept;

        // Destroys every ring: after the frame chain drained and Qt let go of the rings (12 §3.4)
        ~shared_frame_rings();

        shared_frame_rings(const shared_frame_rings&) = delete;
        shared_frame_rings& operator=(const shared_frame_rings&) = delete;

        // Any thread: the size the next ring gets; the latest call wins. The GUI side debounces it (100 ms, 12 §3.3
        // rule 8). A zero side is ignored (a minimised window keeps its ring).
        void request_resize(graphics::texture_extent size) noexcept;

        // The frame chain, at the start of a frame: for a pending size other than the current ring's, creates a ring of
        // generation + 1, makes it current (Qt sees it through qt_current) and retires the previous one. The current
        // ring, or nullptr before the first size. On failure the current ring stays and the size is dropped.
        result<shared_frame_ring*> update();

        shared_frame_ring* current() noexcept
        {
            return current_.get();
        }

        // The Qt render thread: the current ring's state, to attach to
        std::shared_ptr<shared_ring_state> qt_current() const noexcept
        {
            return qt_current_.load();
        }

        // The rings retired and not destroyed yet, oldest first; the frame chain spawns their when_released()
        const std::vector<std::unique_ptr<shared_frame_ring>>& retired() const noexcept
        {
            return retired_;
        }

        // Destroys the retired rings that are released. Returns how many remain.
        std::size_t destroy_released();

    private:
        rhi::sharing sharing_;
        rhi::resources resources_;
        std::atomic<std::uint64_t> pending_size_{ 0 }; // width << 32 | height; 0: none
        std::uint64_t generation_ = 0;
        std::unique_ptr<shared_frame_ring> current_;
        std::vector<std::unique_ptr<shared_frame_ring>> retired_;
        std::atomic<std::shared_ptr<shared_ring_state>> qt_current_;
    };
} // namespace avis::visualizer::rendering

#endif
