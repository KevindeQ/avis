#ifndef AVIS_IO_ASIO_SCHEDULER_H
#define AVIS_IO_ASIO_SCHEDULER_H

#include "avis/error.h"

#include <asio/any_io_executor.hpp>
#include <asio/post.hpp>
#include <stdexec/execution.hpp>

#include <exception>
#include <utility>

namespace avis::io
{
    namespace detail
    {
        template<class Executor = asio::any_io_executor>
        class basic_scheduler
        {
        public:
            using executor_type = Executor;
            using scheduler_concept = stdexec::scheduler_tag;

            template<class ExecutionContext>
                requires std::is_convertible_v<ExecutionContext&, asio::execution_context&>
            explicit basic_scheduler(ExecutionContext& context) noexcept : executor_{ context.get_executor() }
            {}

            template<class OtherExecutor>
            explicit basic_scheduler(OtherExecutor&& executor) noexcept :
                executor_{ std::forward<OtherExecutor>(executor) }
            {}

            bool operator==(const basic_scheduler&) const noexcept = default;

            auto schedule() const noexcept
            {
                return schedule_sender_t{ executor_ };
            }

            executor_type get_executor() const noexcept
            {
                return executor_;
            }

        private:
            struct schedule_sender_t
            {
            public:
                using sender_concept = stdexec::sender_tag;
                using completion_signatures = stdexec::completion_signatures<
                    stdexec::set_value_t(),
                    stdexec::set_error_t(avis::error),
                    stdexec::set_stopped_t()>;

                template<stdexec::receiver Receiver>
                struct operation_state
                {
                    using operation_state_concept = stdexec::operation_state_tag;

                    template<stdexec::receiver OtherReceiver>
                    operation_state(executor_type executor, OtherReceiver&& receiver) noexcept :
                        executor_{ std::move(executor) },
                        receiver_{ std::forward<OtherReceiver>(receiver) }
                    {}

                    operation_state(const operation_state&) = delete;
                    operation_state(operation_state&&) = delete;
                    operation_state& operator=(const operation_state&) = delete;
                    operation_state& operator=(operation_state&&) = delete;

                    void start() & noexcept
                    {
                        if constexpr (!stdexec::unstoppable_token<
                                          stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>>)
                        {
                            const stdexec::stoppable_token auto stop_token =
                                stdexec::get_stop_token(stdexec::get_env(receiver_));
                            if (stop_token.stop_requested())
                            {
                                stdexec::set_stopped(std::move(receiver_));
                                return;
                            }
                        }

                        try
                        {
                            asio::post(executor_, [this]() { stdexec::set_value(std::move(receiver_)); });
                        }
                        catch (...)
                        {
                            stdexec::set_error(std::move(receiver_), avis::make_error(std::current_exception()));
                        }
                    }

                private:
                    executor_type executor_;
                    Receiver receiver_;
                };

                struct environment_t
                {
                public:
                    environment_t(executor_type executor) : executor_{ std::move(executor) } {}

                    template<class CustomizationPointObject>
                    auto query(stdexec::get_completion_scheduler_t<CustomizationPointObject>) const noexcept
                    {
                        return basic_scheduler{ executor_ };
                    }

                private:
                    executor_type executor_;
                };

                schedule_sender_t(executor_type executor) : executor_{ std::move(executor) } {}

                template<stdexec::receiver Receiver>
                auto connect(Receiver&& receiver) &&
                {
                    return operation_state<std::decay_t<Receiver>>{ std::move(executor_),
                                                                    std::forward<Receiver>(receiver) };
                }

                environment_t get_env() const noexcept
                {
                    return environment_t{ executor_ };
                }

            private:
                executor_type executor_;
            };

            executor_type executor_;
        };
    } // namespace detail

    template<class Executor>
    using basic_scheduler = detail::basic_scheduler<Executor>;

    using scheduler = detail::basic_scheduler<>;
} // namespace avis::io

#endif
