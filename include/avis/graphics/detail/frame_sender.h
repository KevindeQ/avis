#ifndef AVIS_GRAPHICS_DETAIL_FRAME_SENDER_H
#define AVIS_GRAPHICS_DETAIL_FRAME_SENDER_H

// The sender behind frame_scheduler::begin_frame (docs/design/17-graphics-frames-and-transfers.md §4, §5): a
// synchronization::when_complete on the last submissions of frame N − F, with the context's recycle on its value path.
// Only the templates that depend on the receiver live here; frame_operation's members are in
// lib/avis/graphics/frame_scheduler.cpp. Free of <windows.h> and D3D12.

#include "avis/error.h"
#include "avis/graphics/frame_token.h"
#include "avis/rhi/synchronization.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <optional>
#include <type_traits>
#include <utility>

namespace avis::graphics
{
    class frame_scheduler;

    namespace detail
    {
        // Non-template part of a begin_frame operation
        class frame_operation
        {
        protected:
            explicit frame_operation(frame_scheduler& scheduler) noexcept : scheduler_{ &scheduler } {}
            ~frame_operation() = default;

            // Marks a begin_frame outstanding. Fails, after a failed debug check, when a frame is already outstanding.
            result<void> begin() noexcept;

            // The wait for the last submissions of the context the next frame uses. Only after begin(); on an error
            // no frame is outstanding any more.
            result<rhi::gpu_wait_sender> wait_for_context() noexcept;

            // The wait completed: recycles the context and makes the token (§5). Errors leave no frame outstanding.
            result<frame_token> deliver() noexcept;

            // The wait was stopped or failed: no frame was started, and the next begin_frame may run
            void abandon() noexcept;

        private:
            frame_scheduler* scheduler_;
        };

        template<class Receiver>
        class frame_operation_state;

        // Receives the GPU wait of a frame_operation_state, and lends it the outer receiver's environment (its stop
        // token above all)
        template<class Receiver>
        struct frame_wait_receiver
        {
            using receiver_concept = stdexec::receiver_tag;

            void set_value() noexcept
            {
                self->on_ready();
            }

            void set_error(avis::error error) noexcept
            {
                self->abandon();
                stdexec::set_error(std::move(self->receiver_), std::move(error));
            }

            void set_stopped() noexcept
            {
                self->abandon();
                stdexec::set_stopped(std::move(self->receiver_));
            }

            stdexec::env_of_t<Receiver> get_env() const noexcept
            {
                return stdexec::get_env(self->receiver_);
            }

            frame_operation_state<Receiver>* self;
        };

        template<class Receiver>
        class frame_operation_state final : public frame_operation
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            frame_operation_state(Receiver receiver, frame_scheduler& scheduler) noexcept(
                std::is_nothrow_move_constructible_v<Receiver>) :
                frame_operation{ scheduler },
                receiver_{ std::move(receiver) }
            {}

            frame_operation_state(const frame_operation_state&) = delete;
            frame_operation_state& operator=(const frame_operation_state&) = delete;

            void start() & noexcept
            {
                if (auto begun = begin(); !begun)
                {
                    stdexec::set_error(std::move(receiver_), std::move(begun.error()));
                    return;
                }

                // Connected in start(), not in connect(): the context's last submissions are only final once the
                // previous frame was ended, which may happen between connect and start
                auto wait = wait_for_context();
                if (!wait)
                {
                    stdexec::set_error(std::move(receiver_), std::move(wait.error()));
                    return;
                }
                wait_.emplace(wait_connector{ this, std::move(*wait) });
                // The wait handles the receiver's stop token itself, through frame_wait_receiver's environment
                stdexec::start(*wait_);
            }

        private:
            friend struct frame_wait_receiver<Receiver>;

            using wait_operation = stdexec::connect_result_t<rhi::gpu_wait_sender, frame_wait_receiver<Receiver>>;

            // Lets std::optional::emplace construct the non-movable wait operation in place
            struct wait_connector
            {
                frame_operation_state* self;
                rhi::gpu_wait_sender sender;

                operator wait_operation() && noexcept
                {
                    return stdexec::connect(std::move(sender), frame_wait_receiver<Receiver>{ self });
                }
            };

            void on_ready() noexcept
            {
                auto token = deliver();
                if (token)
                {
                    stdexec::set_value(std::move(receiver_), std::move(*token));
                }
                else
                {
                    stdexec::set_error(std::move(receiver_), std::move(token.error()));
                }
            }

            Receiver receiver_;
            std::optional<wait_operation> wait_;
        };

        // Completes with set_value(frame_token) once the context of the next frame is free and recycled,
        // set_error(avis::error) with device_lost, or set_stopped() when the receiver's stop token fires before the
        // context is free (no frame is started then). Single-shot; never outlives its scheduler.
        class frame_sender
        {
        public:
            using sender_concept = stdexec::sender_tag;
            using completion_signatures = avis::completions<frame_token>;

            template<stdexec::receiver Receiver>
            auto connect(Receiver receiver) && noexcept(std::is_nothrow_move_constructible_v<Receiver>)
                -> frame_operation_state<Receiver>
            {
                return { std::move(receiver), *scheduler_ };
            }

        private:
            friend class graphics::frame_scheduler;

            explicit frame_sender(frame_scheduler& scheduler) noexcept : scheduler_{ &scheduler } {}

            frame_scheduler* scheduler_;
        };

        static_assert(avis::sender_of<frame_sender, frame_token>);
    } // namespace detail

    using detail::frame_sender;
} // namespace avis::graphics

#endif
