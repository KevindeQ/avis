#ifndef AVIS_GRAPHICS_DETAIL_READBACK_SENDER_H
#define AVIS_GRAPHICS_DETAIL_READBACK_SENDER_H

// The senders behind readback_queue::read_buffer and read_texture (docs/design/17-graphics-frames-and-transfers.md §8):
// start() records and submits the copy into a pooled buffer, then waits with synchronization::when_complete. Only the
// templates that depend on the receiver live here; readback_operation's members are in
// lib/avis/graphics/readback_queue.cpp. Free of <windows.h> and D3D12.

#include "avis/error.h"
#include "avis/graphics/readback_data.h"
#include "avis/rhi/synchronization.h"
#include "avis/rhi/types.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace avis::graphics
{
    class readback_queue;

    namespace detail
    {
        // What a readback copies, and on which queue
        struct readback_request
        {
            rhi::buffer_handle buffer;
            std::uint64_t offset = 0;
            std::uint64_t size = 0;
            rhi::texture_handle texture; // valid for a texture readback
            rhi::subresource subresource;
            rhi::queue_type queue = rhi::queue_type::copy;
        };

        // Non-template part of a readback operation
        class readback_operation
        {
        protected:
            readback_operation(readback_queue& queue, const readback_request& request) noexcept :
                queue_{ &queue },
                request_{ request }
            {}

            ~readback_operation() = default;

            readback_operation(const readback_operation&) = delete;
            readback_operation& operator=(const readback_operation&) = delete;

            // Validates the request, takes a pooled buffer, records the copy and submits it on the request's queue,
            // and returns the wait for it. On an error nothing is outstanding and no buffer is held.
            result<rhi::gpu_wait_sender> submit() noexcept;

            // The copy completed: the lease on the buffer
            readback_data take_data() noexcept;
            readback_image take_image() noexcept;

            // The wait was stopped or failed: the GPU may still write the buffer, so it's destroyed, not pooled
            void abandon() noexcept;

        private:
            // Counts the readback as delivered and hands its buffer to a lease
            readback_lease complete() noexcept;

            readback_queue* queue_;
            readback_request request_;
            rhi::buffer_handle buffer_;
            int size_class_ = -1;
            std::span<const std::byte> bytes_;
            std::uint32_t width_ = 0; // textures
            std::uint32_t height_ = 0;
            std::uint32_t row_pitch_ = 0;
            rhi::format format_ = rhi::format::unknown;
        };

        template<class Receiver, class Value>
        class readback_operation_state;

        // Receives the GPU wait of a readback_operation_state, and lends it the outer receiver's environment (its stop
        // token above all)
        template<class Receiver, class Value>
        struct readback_wait_receiver
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

            readback_operation_state<Receiver, Value>* self;
        };

        template<class Receiver, class Value>
        class readback_operation_state final : public readback_operation
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            readback_operation_state(
                Receiver receiver,
                readback_queue& queue,
                const readback_request& request) noexcept(std::is_nothrow_move_constructible_v<Receiver>) :
                readback_operation{ queue, request },
                receiver_{ std::move(receiver) }
            {}

            void start() & noexcept
            {
                // Nothing is submitted for a readback stopped before it starts
                if (stdexec::get_stop_token(stdexec::get_env(receiver_)).stop_requested())
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                auto wait = submit();
                if (!wait)
                {
                    stdexec::set_error(std::move(receiver_), std::move(wait.error()));
                    return;
                }
                wait_.emplace(wait_connector{ this, std::move(*wait) });
                // The wait handles the receiver's stop token itself, through readback_wait_receiver's environment
                stdexec::start(*wait_);
            }

        private:
            friend struct readback_wait_receiver<Receiver, Value>;

            using wait_operation =
                stdexec::connect_result_t<rhi::gpu_wait_sender, readback_wait_receiver<Receiver, Value>>;

            // Lets std::optional::emplace construct the non-movable wait operation in place
            struct wait_connector
            {
                readback_operation_state* self;
                rhi::gpu_wait_sender sender;

                operator wait_operation() && noexcept
                {
                    return stdexec::connect(std::move(sender), readback_wait_receiver<Receiver, Value>{ self });
                }
            };

            void on_ready() noexcept
            {
                if constexpr (std::is_same_v<Value, readback_image>)
                {
                    stdexec::set_value(std::move(receiver_), take_image());
                }
                else
                {
                    stdexec::set_value(std::move(receiver_), take_data());
                }
            }

            Receiver receiver_;
            std::optional<wait_operation> wait_;
        };

        // Completes with set_value(Value) from poll_completions() once the copy is complete (inline when it already
        // is), set_error(avis::error) for invalid arguments, acquire and submit failures and device_lost, or
        // set_stopped() when the receiver's stop token fires first (the copy still runs; its buffer isn't pooled).
        // Single-shot; never outlives its queue.
        template<class Value>
        class readback_sender
        {
        public:
            using sender_concept = stdexec::sender_tag;
            using completion_signatures = avis::completions<Value>;

            template<stdexec::receiver Receiver>
            auto connect(Receiver receiver) && noexcept(std::is_nothrow_move_constructible_v<Receiver>)
                -> readback_operation_state<Receiver, Value>
            {
                return { std::move(receiver), *queue_, request_ };
            }

        private:
            friend class graphics::readback_queue;

            readback_sender(readback_queue& queue, const readback_request& request) noexcept :
                queue_{ &queue },
                request_{ request }
            {}

            readback_queue* queue_;
            readback_request request_;
        };

        using readback_buffer_sender = readback_sender<readback_data>;
        using readback_texture_sender = readback_sender<readback_image>;

        static_assert(avis::sender_of<readback_buffer_sender, readback_data>);
        static_assert(avis::sender_of<readback_texture_sender, readback_image>);
    } // namespace detail

    using detail::readback_buffer_sender;
    using detail::readback_texture_sender;
} // namespace avis::graphics

#endif
