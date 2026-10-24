#ifndef AVIS_IO_ASIO_USE_SENDER_H
#define AVIS_IO_ASIO_USE_SENDER_H

#include "avis/error.h"
#include "avis/io/detail/registered_operation.h"
#include "avis/io/error.h"

#include <asio/async_result.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/error.hpp>
#include <asio/execution/context.hpp>
#include <asio/execution_context.hpp>
#include <asio/post.hpp>
#include <asio/query.hpp>
#include <asio/strand.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <cctype>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

// asio completion token that turns an asynchronous asio operation into a stdexec sender
// (docs/design/07-io-layer.md §4.6):
//
//   timer.async_wait(avis::io::use_sender)          // set_value() | set_error(avis::error) | set_stopped()
//   file.async_read_some_at(0, buffer, use_sender)   // set_value(std::size_t) | ...
//
// A leading std::error_code in the asio signature is peeled off: operation_aborted completes with set_stopped, any
// other error with set_error(avis::error) in error_domain::io. The sender completes on the thread that ran the asio
// handler (an IO thread) and never hops on its own.

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

            executor_with_default(const InnerExecutor& executor) noexcept : InnerExecutor(executor) {}

            template<class OtherInnerExecutor>
                requires(
                    !std::is_same_v<OtherInnerExecutor, executor_with_default> &&
                    std::is_convertible_v<OtherInnerExecutor, InnerExecutor>)
            executor_with_default(const OtherInnerExecutor& executor) noexcept : InnerExecutor(executor)
            {}
        };

        template<class T>
        using as_default_on_t =
            typename T::template rebind_executor<executor_with_default<typename T::executor_type>>::other;

        template<class T>
        static typename std::decay_t<T>::template rebind_executor<
            executor_with_default<typename std::decay_t<T>::executor_type>>::other
            as_default_on(T&& object)
        {
            return typename std::decay_t<T>::template rebind_executor<
                executor_with_default<typename std::decay_t<T>::executor_type>>::other(std::forward<T>(object));
        }
    };

    inline constexpr use_sender_token use_sender{};

    namespace detail
    {
        template<class T>
        inline constexpr bool is_error_code_v = std::is_same_v<std::decay_t<T>, std::error_code>;

        // Value signature of an asio completion signature void(Args...), without a leading error code
        template<class... Args>
        struct use_sender_traits
        {
            static constexpr bool has_error_code = false;
            using value_signature = stdexec::set_value_t(std::decay_t<Args>...);
        };

        template<class First, class... Rest>
            requires is_error_code_v<First>
        struct use_sender_traits<First, Rest...>
        {
            static constexpr bool has_error_code = true;
            using value_signature = stdexec::set_value_t(std::decay_t<Rest>...);
        };

        // "async_wait" for asio's "...::initiate_async_wait", "asio operation" when the type name doesn't say
        inline std::string operation_name_from(const std::string_view type_name)
        {
            constexpr std::string_view marker = "initiate_";
            std::size_t begin = type_name.rfind(marker);
            if (begin == std::string_view::npos)
            {
                return "asio operation";
            }
            begin += marker.size();

            std::size_t end = begin;
            while (end < type_name.size() &&
                   (std::isalnum(static_cast<unsigned char>(type_name[end])) != 0 || type_name[end] == '_'))
            {
                ++end;
            }
            return end == begin ? std::string{ "asio operation" } : std::string{ type_name.substr(begin, end - begin) };
        }

        template<class Initiation>
        std::string_view operation_name()
        {
            static const std::string name = operation_name_from(typeid(Initiation).name());
            return name;
        }

        // Cancellation needs the I/O object's executor, to serialise signal emits with the completion on a strand
        template<class Initiation>
        concept initiation_with_executor = requires(const Initiation& initiation) { initiation.get_executor(); };

        template<class Initiation>
        using strand_for_t = asio::strand<std::decay_t<decltype(std::declval<const Initiation&>().get_executor())>>;

        // Shared between a cancellable operation and the emits posted for it, so a posted emit never touches an
        // operation that has already completed. `completed` is only read and written on the strand.
        template<class Strand>
        struct cancellation_state
        {
            explicit cancellation_state(Strand executor) : strand{ std::move(executor) } {}

            Strand strand;
            asio::cancellation_signal signal;
            bool completed = false;
            std::atomic<bool> requested{ false };
            std::atomic<bool> initiated{ false };
        };

        template<class Strand>
        void post_emit(const std::shared_ptr<cancellation_state<Strand>>& state) noexcept
        {
            try
            {
                asio::post(
                    state->strand,
                    [state]()
                    {
                        if (!state->completed)
                        {
                            state->signal.emit(asio::cancellation_type::total);
                        }
                    });
            }
            catch (...)
            {
                // Allocation failure: the operation then runs to its natural completion
            }
        }

        // Stop callback: emits only once the operation is initiated, so the emit never races with asio connecting the
        // cancellation slot. start() posts the emit itself when the stop request came in before.
        template<class Strand>
        struct stop_request
        {
            std::shared_ptr<cancellation_state<Strand>> state;

            void operator()() const noexcept
            {
                state->requested.store(true);
                if (state->initiated.load())
                {
                    post_emit(state);
                }
            }
        };

        // Completion handler of an operation that can't be cancelled after it started
        template<class Operation>
        struct plain_handler
        {
            Operation* operation;

            template<class... HandlerArgs>
            void operator()(HandlerArgs&&... arguments)
            {
                operation->complete(std::forward<HandlerArgs>(arguments)...);
            }
        };

        // Completion handler of a cancellable operation: runs on the operation's strand and carries its cancellation
        // slot
        template<class Operation, class Strand>
        struct cancellable_handler
        {
            using executor_type = Strand;
            using cancellation_slot_type = asio::cancellation_slot;

            Operation* operation;
            Strand strand;
            asio::cancellation_slot slot;

            executor_type get_executor() const noexcept
            {
                return strand;
            }

            cancellation_slot_type get_cancellation_slot() const noexcept
            {
                return slot;
            }

            template<class... HandlerArgs>
            void operator()(HandlerArgs&&... arguments)
            {
                operation->complete(std::forward<HandlerArgs>(arguments)...);
            }
        };
    } // namespace detail

    // Every operation whose initiation exposes an executor is cancellable: by its receiver's stop token when that can
    // stop, and by io_context::request_stop() through the operation registry (docs/design/07-io-layer.md §3.3)
    template<class Receiver, class Initiation, class Arguments, class... Args>
    class use_sender_operation final : private detail::registered_operation
    {
    public:
        using operation_state_concept = stdexec::operation_state_tag;

        use_sender_operation(Receiver receiver, Initiation initiation, Arguments arguments) :
            receiver_{ std::move(receiver) },
            initiation_{ std::move(initiation) },
            arguments_{ std::move(arguments) }
        {}

        use_sender_operation(const use_sender_operation&) = delete;
        use_sender_operation(use_sender_operation&&) = delete;
        use_sender_operation& operator=(const use_sender_operation&) = delete;
        use_sender_operation& operator=(use_sender_operation&&) = delete;

        void start() & noexcept
        {
            if constexpr (cancellable)
            {
                start_cancellable();
            }
            else
            {
                try
                {
                    initiate(detail::plain_handler<use_sender_operation>{ this });
                }
                catch (...)
                {
                    stdexec::set_error(std::move(receiver_), avis::make_error(std::current_exception()));
                }
            }
        }

        // Called by the completion handler, on the strand for cancellable operations
        template<class... HandlerArgs>
        void complete(HandlerArgs&&... arguments) noexcept
        {
            if constexpr (cancellable)
            {
                // First, so the registry never calls cancel() while the members below change
                if (registry_ != nullptr)
                {
                    detail::remove_operation(*registry_, *this);
                    registry_ = nullptr;
                }
                if (cancellation_)
                {
                    cancellation_->completed = true;
                }
                if constexpr (stoppable)
                {
                    stop_callback_.reset(); // waits for a stop callback that is running on another thread
                }
                cancellation_.reset();
            }
            deliver(std::forward<HandlerArgs>(arguments)...);
        }

    private:
        using traits = detail::use_sender_traits<Args...>;
        using stop_token_type = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

        static constexpr bool cancellable = detail::initiation_with_executor<Initiation>;
        static constexpr bool stoppable = cancellable && !stdexec::unstoppable_token<stop_token_type>;

        // Members that only cancellable (and stoppable) operations use; placeholders otherwise
        template<bool Cancellable, class = void>
        struct cancellation_members
        {
            using state = std::shared_ptr<void>;
        };

        template<class Unused>
        struct cancellation_members<true, Unused>
        {
            using strand_type = detail::strand_for_t<Initiation>;
            using state = std::shared_ptr<detail::cancellation_state<strand_type>>;
        };

        template<bool Stoppable, class = void>
        struct stop_members
        {
            using callback = std::optional<int>;
        };

        template<class Unused>
        struct stop_members<true, Unused>
        {
            using callback = std::optional<stdexec::stop_callback_for_t<
                stop_token_type,
                detail::stop_request<typename cancellation_members<true>::strand_type>>>;
        };

        // Registry cancellation: called under the registry's lock, only between add and remove
        void cancel() noexcept override
        {
            if constexpr (cancellable)
            {
                if (cancellation_)
                {
                    detail::stop_request<typename cancellation_members<true>::strand_type>{ cancellation_ }();
                }
            }
        }

        template<class Handler>
        void initiate(Handler handler)
        {
            std::apply(
                [this, &handler](auto&... arguments)
                { std::move(initiation_)(std::move(handler), std::move(arguments)...); },
                arguments_);
        }

        void start_cancellable() noexcept
        {
            using strand_type = detail::strand_for_t<Initiation>;
            using state_type = detail::cancellation_state<strand_type>;

            [[maybe_unused]] const stop_token_type stop_token = stdexec::get_stop_token(stdexec::get_env(receiver_));
            if constexpr (stoppable)
            {
                if (stop_token.stop_requested())
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
            }

            std::shared_ptr<state_type> state;
            try
            {
                state = std::make_shared<state_type>(asio::make_strand(initiation_.get_executor()));
                cancellation_ = state;

                // Register before starting, so request_stop() either sees this operation or refuses it
                detail::operation_registry* const registry =
                    detail::find_operation_registry(asio::query(initiation_.get_executor(), asio::execution::context));
                if (registry != nullptr)
                {
                    if (!detail::add_operation(*registry, *this))
                    {
                        cancellation_.reset();
                        stdexec::set_stopped(std::move(receiver_)); // the context is stopping
                        return;
                    }
                    registry_ = registry;
                }

                if constexpr (stoppable)
                {
                    stop_callback_.emplace(stop_token, detail::stop_request<strand_type>{ state });
                }
                initiate(
                    detail::cancellable_handler<use_sender_operation, strand_type>{
                        this, state->strand, state->signal.slot() });
            }
            catch (...)
            {
                if constexpr (stoppable)
                {
                    stop_callback_.reset();
                }
                if (registry_ != nullptr)
                {
                    detail::remove_operation(*registry_, *this);
                    registry_ = nullptr;
                }
                cancellation_.reset();
                stdexec::set_error(std::move(receiver_), avis::make_error(std::current_exception()));
                return;
            }

            // The operation may already have completed on an IO thread, so only the local state is used from here on
            state->initiated.store(true);
            if (state->requested.load())
            {
                detail::post_emit(state);
            }
        }

        template<class... HandlerArgs>
        void deliver(HandlerArgs&&... arguments) noexcept
        {
            if constexpr (traits::has_error_code)
            {
                deliver_with_error_code(std::forward<HandlerArgs>(arguments)...);
            }
            else
            {
                stdexec::set_value(std::move(receiver_), std::forward<HandlerArgs>(arguments)...);
            }
        }

        template<class ErrorCode, class... Rest>
        void deliver_with_error_code(const ErrorCode& code, Rest&&... rest) noexcept
        {
            if (code == asio::error::operation_aborted)
            {
                stdexec::set_stopped(std::move(receiver_));
            }
            else if (code)
            {
                deliver_error(code);
            }
            else
            {
                stdexec::set_value(std::move(receiver_), std::forward<Rest>(rest)...);
            }
        }

        void deliver_error(const std::error_code& code) noexcept
        {
            try
            {
                stdexec::set_error(
                    std::move(receiver_), io::make_error(code, detail::operation_name<Initiation>(), {}));
            }
            catch (...)
            {
                // Formatting the message failed (allocation): report the error without it
                stdexec::set_error(
                    std::move(receiver_),
                    avis::error{ .domain = error_domain::io,
                                 .code = static_cast<std::int32_t>(error_kind::system_error) });
            }
        }

        Receiver receiver_;
        Initiation initiation_;
        Arguments arguments_;
        typename cancellation_members<cancellable>::state cancellation_;
        typename stop_members<stoppable>::callback stop_callback_;
        detail::operation_registry* registry_ = nullptr;
    };

    template<class Initiation, class Arguments, class... Args>
    class use_sender_sender
    {
    public:
        using sender_concept = stdexec::sender_tag;
        using completion_signatures = stdexec::completion_signatures<
            typename detail::use_sender_traits<Args...>::value_signature,
            stdexec::set_error_t(avis::error),
            stdexec::set_stopped_t()>;

        use_sender_sender(Initiation initiation, Arguments arguments) :
            initiation_{ std::move(initiation) },
            arguments_{ std::move(arguments) }
        {}

        template<stdexec::receiver Receiver>
        auto connect(Receiver receiver) && -> use_sender_operation<Receiver, Initiation, Arguments, Args...>
        {
            return { std::move(receiver), std::move(initiation_), std::move(arguments_) };
        }

    private:
        Initiation initiation_;
        Arguments arguments_;
    };
} // namespace avis::io

namespace asio
{
    template<class... Args>
    struct async_result<avis::io::use_sender_token, void(Args...)>
    {
        template<class Initiation, class... InitiationArgs>
        static auto initiate(Initiation&& initiation, avis::io::use_sender_token, InitiationArgs&&... arguments)
        {
            using arguments_type = std::tuple<std::decay_t<InitiationArgs>...>;
            return avis::io::use_sender_sender<std::decay_t<Initiation>, arguments_type, Args...>{
                std::forward<Initiation>(initiation), arguments_type{ std::forward<InitiationArgs>(arguments)... }
            };
        }
    };
} // namespace asio

#endif
