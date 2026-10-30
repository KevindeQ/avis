#ifndef AVIS_GRAPHICS_DETAIL_UPLOAD_SENDER_H
#define AVIS_GRAPHICS_DETAIL_UPLOAD_SENDER_H

// The sender behind upload_queue::upload_buffer_async and upload_texture_async (docs/design/17-graphics-frames-and-
// transfers.md §7.2). The operation state is an intrusive FIFO entry of the queue: it waits without a thread for ring
// space, and the queue's space wait resumes it from poll_completions(). upload_waiter's members are in
// lib/avis/graphics/upload_queue.cpp. Free of <windows.h> and D3D12.

#include "avis/detail/receiver_stop_callback.h"
#include "avis/error.h"
#include "avis/graphics/upload_ticket.h"
#include "avis/rhi/types.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace avis::graphics
{
    class upload_queue;

    namespace detail
    {
        struct upload_queue_state;

        // What an async upload copies; the spans must stay valid until it completes
        struct upload_request
        {
            enum class kind : std::uint8_t
            {
                buffer,
                texture
            };

            kind type = kind::buffer;
            rhi::buffer_handle buffer;
            std::uint64_t buffer_offset = 0;
            std::span<const std::byte> bytes;
            rhi::texture_handle texture;
            std::span<const subresource_data> subresources;
        };

        // Non-template part of an async upload: a FIFO entry of its queue
        class upload_waiter
        {
        public:
            // Unlinked by the space wait, outside the queue's lock: retry (back to the front if the space is gone)
            virtual void resume() noexcept = 0;
            // The space wait failed (device_lost): complete with the error
            virtual void fail(avis::error error) noexcept = 0;

        protected:
            enum class attempt_outcome : std::uint8_t
            {
                completed, // `outcome` holds the ticket or the error
                queued, // waiting for space; the operation may already be complete and gone
                stopped // a stop request came first
            };

            upload_waiter(upload_queue& queue, const upload_request& request) noexcept :
                queue_{ &queue },
                request_{ request }
            {}

            ~upload_waiter() = default;

            upload_waiter(const upload_waiter&) = delete;
            upload_waiter& operator=(const upload_waiter&) = delete;

            // The synchronous path; on upload_queue_full the FIFO (at the front for a resumed waiter)
            attempt_outcome attempt(bool at_front, result<upload_ticket>& outcome) noexcept;

            // From the stop callback, any thread: true when this call unlinked the waiter, so the caller completes with
            // set_stopped. False when it wasn't queued: its attempt or resume sees the stop request instead.
            bool cancel() noexcept;

        private:
            friend struct upload_queue_state;
            friend class graphics::upload_queue;

            upload_queue* queue_;
            upload_request request_;
            std::uint64_t staged_size_ = 0; // ring bytes the upload needs, known after its first attempt
            std::atomic<bool> stop_requested_{ false }; // set before cancel() takes the queue's lock
            upload_waiter* next_ = nullptr; // the FIFO, guarded by the queue's mutex
            upload_waiter* previous_ = nullptr;
            bool linked_ = false;
        };

        template<class Receiver>
        class upload_operation_state final : public upload_waiter
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            upload_operation_state(Receiver receiver, upload_queue& queue, const upload_request& request) noexcept(
                std::is_nothrow_move_constructible_v<Receiver>) :
                upload_waiter{ queue, request },
                receiver_{ std::move(receiver) }
            {}

            void start() & noexcept
            {
                if (stop_callback::stop_requested(receiver_))
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                // Installed first, so a stop during the first attempt is seen when the waiter would be queued
                stop_callback_.install(receiver_, stop_function{ this });
                run(false);
            }

            void resume() noexcept override
            {
                run(true);
            }

            void fail(avis::error error) noexcept override
            {
                stop_callback_.reset();
                stdexec::set_error(std::move(receiver_), std::move(error));
            }

        private:
            struct stop_function
            {
                upload_operation_state* self;

                void operator()() const noexcept
                {
                    if (self->cancel())
                    {
                        self->stop_callback_.reset();
                        stdexec::set_stopped(std::move(self->receiver_));
                    }
                }
            };

            using stop_callback = avis::detail::receiver_stop_callback<Receiver, stop_function>;

            void run(const bool at_front) noexcept
            {
                result<upload_ticket> outcome;
                switch (attempt(at_front, outcome))
                {
                case attempt_outcome::queued:
                    return; // touch nothing: the space wait may already have resumed and completed it
                case attempt_outcome::stopped:
                    stop_callback_.reset();
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                case attempt_outcome::completed:
                    stop_callback_.reset();
                    if (outcome)
                    {
                        stdexec::set_value(std::move(receiver_), *outcome);
                    }
                    else
                    {
                        stdexec::set_error(std::move(receiver_), std::move(outcome.error()));
                    }
                    return;
                }
            }

            Receiver receiver_;
            stop_callback stop_callback_;
        };

        // Completes with set_value(upload_ticket) once the data is in the ring (inline when it fits now),
        // set_error(avis::error) for the synchronous path's errors other than upload_queue_full, or set_stopped()
        // when the receiver's stop token fires while it waits. Single-shot; never outlives its queue.
        class upload_sender
        {
        public:
            using sender_concept = stdexec::sender_tag;
            using completion_signatures = avis::completions<upload_ticket>;

            template<stdexec::receiver Receiver>
            auto connect(Receiver receiver) && noexcept(std::is_nothrow_move_constructible_v<Receiver>)
                -> upload_operation_state<Receiver>
            {
                return { std::move(receiver), *queue_, request_ };
            }

        private:
            friend class graphics::upload_queue;

            upload_sender(upload_queue& queue, const upload_request& request) noexcept :
                queue_{ &queue },
                request_{ request }
            {}

            upload_queue* queue_;
            upload_request request_;
        };

        static_assert(avis::sender_of<upload_sender, upload_ticket>);
    } // namespace detail

    using detail::upload_sender;
} // namespace avis::graphics

#endif
