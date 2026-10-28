#ifndef AVIS_RHI_DETAIL_GPU_WAIT_SENDER_H
#define AVIS_RHI_DETAIL_GPU_WAIT_SENDER_H

// The sender behind synchronization::when_complete (docs/design/09-rhi.md §5.1, §16, §23.5). Only the templates that
// depend on the receiver live here; gpu_wait_operation's members are in lib/avis/rhi/d3d12/gpu_wait_operation.cpp.
// No allocation and no kernel object per wait: the waiter lives in the operation state. Free of <windows.h>.

#include "avis/detail/receiver_stop_callback.h"
#include "avis/error.h"
#include "avis/rhi/detail/completion_waiter.h"
#include "avis/sender_of.h"

#include <stdexec/execution.hpp>

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace avis::rhi
{
    class synchronization;

    namespace d3d12
    {
        class device_state;
    } // namespace d3d12

    namespace detail
    {
        // Non-template part of a GPU wait
        class gpu_wait_operation : public completion_waiter
        {
        protected:
            enum class start_outcome : std::uint8_t
            {
                added, // queued on the tracker: completes from poll_completions() or a stop request
                reached, // every value already reached: complete inline with set_value
                failed, // device lost: complete inline with set_error(error)
                stopped // a stop request came first: complete inline with set_stopped
            };

            gpu_wait_operation(d3d12::device_state& state, const std::array<std::uint64_t, 3>& values) noexcept;
            ~gpu_wait_operation() = default;

            // Drops the values that are already reached, then adds the waiter to the tracker
            start_outcome start_wait(avis::error& error) noexcept;

            // From the stop callback, any thread: true when this call removed the waiter, so the caller completes
            // with set_stopped. False when poll_completions() already took it (its completion is on the way) or it
            // wasn't added yet (start_wait then reports stopped).
            bool cancel_wait() noexcept;

        private:
            d3d12::device_state* state_;
        };

        template<class Receiver>
        class gpu_wait_operation_state final : public gpu_wait_operation
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            gpu_wait_operation_state(
                Receiver receiver,
                d3d12::device_state& state,
                const std::array<std::uint64_t, 3>& values) noexcept(std::is_nothrow_move_constructible_v<Receiver>) :
                gpu_wait_operation{ state, values },
                receiver_{ std::move(receiver) }
            {}

            gpu_wait_operation_state(const gpu_wait_operation_state&) = delete;
            gpu_wait_operation_state& operator=(const gpu_wait_operation_state&) = delete;

            void start() & noexcept
            {
                if (stop_callback::stop_requested(receiver_))
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }

                // Installed first, so a stop between here and the tracker's add() isn't lost: the tracker marks the
                // waiter cancelled and add() reports it
                stop_callback_.install(receiver_, stop_function{ this });

                avis::error error;
                switch (start_wait(error))
                {
                case start_outcome::added:
                    return; // the operation may already be complete and gone: touch nothing
                case start_outcome::reached:
                    on_reached();
                    return;
                case start_outcome::failed:
                    on_failed(std::move(error));
                    return;
                case start_outcome::stopped:
                    on_stopped();
                    return;
                }
            }

            void on_reached() noexcept override
            {
                stop_callback_.reset();
                stdexec::set_value(std::move(receiver_));
            }

            void on_failed(avis::error error) noexcept override
            {
                stop_callback_.reset();
                stdexec::set_error(std::move(receiver_), std::move(error));
            }

            void on_stopped() noexcept override
            {
                stop_callback_.reset();
                stdexec::set_stopped(std::move(receiver_));
            }

        private:
            struct stop_function
            {
                gpu_wait_operation_state* self;

                void operator()() const noexcept
                {
                    if (self->cancel_wait())
                    {
                        self->on_stopped();
                    }
                }
            };

            using stop_callback = avis::detail::receiver_stop_callback<Receiver, stop_function>;

            Receiver receiver_;
            stop_callback stop_callback_;
        };

        // Completes with set_value() once every point is reached, set_error(device_lost) after device removal, or
        // set_stopped() when the receiver's stop token fires first (§5.1). Copyable; never outlives its RHI.
        class gpu_wait_sender
        {
        public:
            using sender_concept = stdexec::sender_tag;
            using completion_signatures = avis::completions<>;

            template<stdexec::receiver Receiver>
            auto connect(Receiver receiver) const -> gpu_wait_operation_state<Receiver>
            {
                return { std::move(receiver), *state_, values_ };
            }

        private:
            friend class rhi::synchronization;

            gpu_wait_sender(d3d12::device_state& state, const std::array<std::uint64_t, 3>& values) noexcept :
                state_{ &state },
                values_{ values }
            {}

            d3d12::device_state* state_;
            std::array<std::uint64_t, 3> values_; // the latest value per queue, indexed by queue_type
        };

        static_assert(avis::sender_of<gpu_wait_sender>);
    } // namespace detail

    using detail::gpu_wait_sender;
} // namespace avis::rhi

#endif
