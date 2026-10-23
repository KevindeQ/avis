#ifndef AVIS_IO_ASIO_USE_SENDER_H
#define AVIS_IO_ASIO_USE_SENDER_H

#include <asio/async_result.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <stdexec/execution.hpp>

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace avis::io
{
    class use_sender_token
    {
    public:
        constexpr use_sender_token() {}

        template<class InnerExecutor>
        struct executor_with_default : InnerExecutor
        {
            using default_completion_token_type = use_sender_token;

            executor_with_default(const InnerExecutor& ex) noexcept : InnerExecutor(ex) {}

            template<class OtherInnerExecutor>
                requires(
                    !std::is_same_v<OtherInnerExecutor, executor_with_default> &&
                    std::is_convertible_v<OtherInnerExecutor, InnerExecutor>)
            executor_with_default(const OtherInnerExecutor& ex) noexcept : InnerExecutor(ex)
            {}
        };

        template<class T>
        using as_default_on_t =
            typename T::template rebind_executor<executor_with_default<typename T::executor_type>>::other;

        template<class T>
        static typename std::decay_t<T>::template rebind_executor<
            executor_with_default<typename std::decay_t<T>::executor_type>>::other
            as_default_on(T&& obj)
        {
            return typename std::decay_t<T>::template rebind_executor<
                executor_with_default<typename std::decay_t<T>::executor_type>>::other(std::forward<T>(obj));
        }
    };

    inline constexpr use_sender_token use_sender{};

    namespace detail
    {
        template<class Initiation, class... Args>
        struct initializer
        {
            template<class SpecificInitiation, class... SpecificArgs>
            explicit initializer(SpecificInitiation&& init, SpecificArgs&&... args) :
                initiation_{ std::forward<SpecificInitiation>(init) },
                arguments_{ std::forward<SpecificArgs>(args)... }
            {}

            initializer(const initializer&) = delete;
            initializer(initializer&&) = default;
            initializer& operator=(const initializer&) = delete;
            initializer& operator=(initializer&&) = default;

            template<class Handler>
            void operator()(Handler&& handler) &&
            {
                std::apply(
                    [this, h = std::forward<Handler>(handler)](Args&&... args) mutable
                    { std::move(initiation_)(std::move(h), std::move(args)...); },
                    std::move(arguments_));
            }

        private:
            Initiation initiation_;
            std::tuple<Args...> arguments_;
        };

        template<class... Args>
        class base_operation_state
        {
        public:
            base_operation_state() = default;
            base_operation_state(const base_operation_state&) = delete;
            base_operation_state(base_operation_state&&) = delete;
            base_operation_state& operator=(const base_operation_state&) = delete;
            base_operation_state& operator=(base_operation_state&&) = delete;

            virtual void complete(Args&&... arguments) noexcept = 0;
        };

        template<class... Args>
        struct basic_sender_handler
        {
        public:
            basic_sender_handler(base_operation_state<Args...>* op_state) : op_state_{ op_state } {}

            template<class... HandlerArgs>
            void operator()(HandlerArgs... arguments)
            {
                op_state_->complete(std::forward<HandlerArgs>(arguments)...);
            }

        private:
            base_operation_state<Args...>* op_state_;
        };

        template<stdexec::receiver Receiver, class Initializer, class... Args>
        class basic_operation_state : private base_operation_state<Args...>
        {
        public:
            using initializer_type = Initializer;

            basic_operation_state(Receiver&& receiver, initializer_type&& initializer) :
                receiver_{ std::move(receiver) },
                initializer_{ std::move(initializer) }
            {}

            void initiate()
            {
                std::move(initializer_)(basic_sender_handler<Args...>{ this });
            }

            void set_error() noexcept
            {
                stdexec::set_error(std::move(receiver_), std::current_exception());
            }

            void stop() noexcept
            {
                stdexec::set_stopped(std::move(receiver_));
            }

            void complete(Args&&... arguments) noexcept override
            {
                if constexpr (sizeof...(arguments) == 0)
                {
                    stdexec::set_value(std::move(receiver_));
                }
                else
                {
                    // TODO: Implement support for cancellation
                    stdexec::set_value(std::move(receiver_), std::move(arguments)...);
                }
            }

        protected:
            auto get_stop_token() const
            {
                return stdexec::get_stop_token(stdexec::get_env(receiver_));
            }

        private:
            Receiver receiver_;
            initializer_type initializer_;
        };
    } // namespace detail

    template<class Initializer, class... Args>
    class transfer_sender
    {
    public:
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(Args...),
            stdexec::set_error_t(std::exception_ptr),
            stdexec::set_stopped_t()>;

        using initializer_type = Initializer;

        template<stdexec::receiver Receiver>
        class operation_state final : private detail::basic_operation_state<Receiver, initializer_type, Args...>
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            operation_state(Receiver&& receiver, initializer_type&& initializer) :
                detail::basic_operation_state<Receiver, initializer_type, Args...>::basic_operation_state(
                    std::forward<Receiver>(receiver), std::forward<initializer_type>(initializer))
            {}

            void start() & noexcept
            {
                const auto stop_token = this->get_stop_token();
                if (stop_token.stop_requested())
                {
                    this->stop();
                    return;
                }

                // Setup stop signal callback for when operation is enqueued
                stop_callback_.emplace(stop_token, stop_callback_impl{ this });

                // Operation was already started by previous invocation of start. Stop the current invocation.
                execution_state expected = execution_state::construction;
                if (!current_execution_state_.compare_exchange_strong(
                        expected, execution_state::emplaced, std::memory_order_acq_rel))
                {
                    stop_callback_.reset();
                    this->stop();

                    return;
                }

                try
                {
                    // Enqueue I/O operation
                    this->initiate();
                }
                catch (...)
                {
                    stop_callback_.reset();
                    this->set_error();

                    return;
                }

                expected = execution_state::emplaced;
                if (!current_execution_state_.compare_exchange_strong(
                        expected, execution_state::initiated, std::memory_order_acq_rel))
                {
                    // Cancellation has been requested, because the current_execution_state_ has been modified after
                    // the call to initiate, but `stop_callback` does not emit a cancellation signal until after the
                    // call start has been completed (see the `if` branch in `stop_callback_impl::operator()`).
                    stop_callback_.reset();
                    signal_.emit(asio::cancellation_type::total);

                    return;
                }
            }

        private:
            void complete(Args&&... args) noexcept override
            {
                stop_callback_.reset();

                using base = detail::basic_operation_state<Receiver, initializer_type, Args...>;
                base::complete(std::forward(args)...);
            }

            asio::cancellation_signal signal_;

            enum struct execution_state : char
            {
                constructed,
                emplaced,
                initiated,
                stopped
            };
            std::atomic<execution_state> current_execution_state_{ execution_state::constructed };

            struct stop_callback_impl
            {
            public:
                operation_state* op_state;

                void operator()() noexcept
                {
                    // Check if the current execution is in charge of updating the state tracking variable
                    // current_execution_state_. If, so set the execution state to stopped because a stop signal was
                    // received.
                    execution_state expected = op_state->current_execution_state_.load(std::memory_order_relaxed);
                    while (!op_state->current_execution_state_.compare_exchange_weak(
                        expected, execution_state::stopped, std::memory_order_acq_rel))
                    {
                    }

                    // Stop was requested after the call to start of the operation_state has completed
                    if (expected == execution_state::initiated)
                    {
                        op_state->signal_.emit(asio::cancellation_type::total);
                    }
                }
            };
            using stop_callback_t = typename stdexec::stop_token_of_t<
                stdexec::env_of_t<Receiver>&>::template callback_type<stop_callback_impl>;
            std::optional<stop_callback_t> stop_callback_;
        };

        template<stdexec::receiver Receiver>
        class operation_state_no_cancellation final :
            private detail::basic_operation_state<Receiver, initializer_type, Args...>
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            operation_state_no_cancellation(Receiver&& receiver, initializer_type&& initializer) :
                detail::basic_operation_state<Receiver, initializer_type, Args...>::basic_operation_state(
                    std::forward<Receiver>(receiver), std::forward<initializer_type>(initializer))
            {}

            void start() & noexcept
            {
                try
                {
                    this->initiate();
                }
                catch (...)
                {
                    this->set_error();
                }
            }
        };

        transfer_sender(initializer_type&& initializer) : initializer_{ std::forward<initializer_type>(initializer) } {}

        template<stdexec::receiver Receiver>
        stdexec::operation_state auto connect(Receiver&& receiver) &&
        {
            if constexpr (stdexec::unstoppable_token<stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>>)
            {
                return operation_state_no_cancellation<std::decay_t<Receiver>>{ std::forward<Receiver>(receiver),
                                                                                std::move(initializer_) };
            }
            else
            {
                return operation_state<std::decay_t<Receiver>>{ std::forward<Receiver>(receiver),
                                                                std::move(initializer_) };
            }
        }

    private:
        initializer_type initializer_;
    };

    template<class Initializer, class... Args>
    class sender
    {
    public:
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(Args...),
            stdexec::set_error_t(std::exception_ptr),
            stdexec::set_stopped_t()>;

        using initializer_type = Initializer;

        template<stdexec::receiver Receiver>
        class operation_state final : private detail::basic_operation_state<Receiver, initializer_type, Args...>
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            operation_state(Receiver&& receiver, initializer_type&& initializer) :
                detail::basic_operation_state<Receiver, initializer_type, Args...>::basic_operation_state(
                    std::forward<Receiver>(receiver), std::forward<initializer_type>(initializer))
            {}

            void start() & noexcept
            {
                const auto stop_token = this->get_stop_token();
                if (stop_token.stop_requested())
                {
                    this->stop();
                    return;
                }

                // Setup stop signal callback for when operation is enqueued
                stop_callback_.emplace(stop_token, stop_callback_impl{ this });

                // Operation was already started by previous invocation of start. Stop the current invocation.
                execution_state expected = execution_state::construction;
                if (!current_execution_state_.compare_exchange_strong(
                        expected, execution_state::emplaced, std::memory_order_acq_rel))
                {
                    stop_callback_.reset();
                    this->stop();

                    return;
                }

                try
                {
                    // Enqueue I/O operation
                    this->initiate();
                }
                catch (...)
                {
                    stop_callback_.reset();
                    this->set_error();

                    return;
                }

                expected = execution_state::emplaced;
                if (!current_execution_state_.compare_exchange_strong(
                        expected, execution_state::initiated, std::memory_order_acq_rel))
                {
                    // Cancellation has been requested, because the current_execution_state_ has been modified after
                    // the call to initiate, but `stop_callback` does not emit a cancellation signal until after the
                    // call start has been completed (see the `if` branch in `stop_callback_impl::operator()`).
                    stop_callback_.reset();
                    signal_.emit(asio::cancellation_type_t::total);

                    return;
                }
            }

        private:
            void complete(Args&&... args) noexcept override
            {
                stop_callback_.reset();

                using base = detail::basic_operation_state<Receiver, initializer_type, Args...>;
                base::complete(std::forward(args)...);
            }

            asio::cancellation_signal signal_;

            enum struct execution_state : char
            {
                constructed,
                emplaced,
                initiated,
                stopped
            };
            std::atomic<execution_state> current_execution_state_{ execution_state::constructed };

            struct stop_callback_impl
            {
            public:
                operation_state* op_state;

                void operator()() noexcept
                {
                    // Check if the current execution is in charge of updating the state tracking variable
                    // current_execution_state_. If, so set the execution state to stopped because a stop signal was
                    // received.
                    execution_state expected = op_state->current_execution_state_.load(std::memory_order_relaxed);
                    while (!op_state->current_execution_state_.compare_exchange_weak(
                        expected, execution_state::stopped, std::memory_order_acq_rel))
                    {
                    }

                    // Stop was requested after the call to start of the operation_state has completed
                    if (expected == execution_state::initiated)
                    {
                        op_state->signal_.emit(asio::cancellation_type_t::total);
                    }
                }
            };
            using stop_callback_t = typename stdexec::stop_token_of_t<
                stdexec::env_of_t<Receiver>&>::template callback_type<stop_callback_impl>;
            std::optional<stop_callback_t> stop_callback_;
        };

        template<stdexec::receiver Receiver>
        struct operation_state_no_cancellation final :
            private detail::basic_operation_state<Receiver, initializer_type, Args...>
        {
        public:
            using operation_state_concept = stdexec::operation_state_tag;

            operation_state_no_cancellation(Receiver&& receiver, initializer_type&& initializer) :
                detail::basic_operation_state<Receiver, initializer_type, Args...>::basic_operation_state(
                    std::forward<Receiver>(receiver), std::forward<initializer_type>(initializer))
            {}

            void start() & noexcept
            {
                try
                {
                    this->initiate();
                }
                catch (...)
                {
                    this->set_error();
                }
            }
        };

        sender(initializer_type&& initializer) : initializer_{ std::forward<initializer_type>(initializer) } {}

        template<stdexec::receiver Receiver>
        stdexec::operation_state auto connect(Receiver&& receiver) &&
        {
            // Check if the operation needs to be moved to a different scheduler
            const auto& env = stdexec::get_env(receiver);
            if constexpr (requires { stdexec::get_scheduler(env); })
            {
                return stdexec::connect(
                    stdexec::continues_on(
                        transfer_sender<initializer_type, Args...>{ std::move(initializer_) },
                        stdexec::get_scheduler(env)),
                    std::forward<Receiver>(receiver));
            }

            // Check if cancellation needs to be supported. If not, return a non-cancellable operation state.
            else if constexpr (stdexec::unstoppable_token<stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>>)
            {
                return operation_state_no_cancellation<std::decay_t<Receiver>>{ std::forward<Receiver>(receiver),
                                                                                std::move(initializer_) };
            }

            else
            {
                return operation_state<std::decay_t<Receiver>>{ std::forward<Receiver>(receiver),
                                                                std::move(initializer_) };
            }
        }

    private:
        initializer_type initializer_;
    };
} // namespace avis::io

namespace asio
{
    template<class... Args>
    struct async_result<avis::io::use_sender_token, void(Args...)>
    {
        template<typename Initiation, typename... InitiationArgs>
        static auto initiate(Initiation&& initiation, avis::io::use_sender_token, InitiationArgs&&... args)
        {
            using initializer_type =
                avis::io::detail::initializer<std::decay_t<Initiation>, std::decay_t<InitiationArgs>...>;

            return avis::io::sender<initializer_type, Args...>{ initializer_type{
                std::forward<Initiation>(initiation), std::forward<InitiationArgs>(args)... } };
        }
    };
} // namespace asio

#endif
