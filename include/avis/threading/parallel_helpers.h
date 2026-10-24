#ifndef AVIS_THREADING_PARALLEL_HELPERS_H
#define AVIS_THREADING_PARALLEL_HELPERS_H

#include "avis/error.h"

#include <stdexec/execution.hpp>

#include <concepts>
#include <exception>
#include <expected>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

// Helpers for pipelines on stdexec::get_parallel_scheduler()

namespace avis::threading
{
    namespace detail
    {
        // Marks the calling thread as running parallel work while alive, for detail::in_parallel_work() and
        // avis::threading::sync_wait. Used by parallel_backend around every completion and bulk item it runs on a
        // worker; other code shouldn't use it, since a wrong mark changes what sync_wait accepts. Defined in
        // parallel_helpers.cpp next to the rest of the per-thread state.
        class parallel_work_scope
        {
        public:
            parallel_work_scope() noexcept;
            ~parallel_work_scope();
            parallel_work_scope(const parallel_work_scope&) = delete;
            parallel_work_scope& operator=(const parallel_work_scope&) = delete;
        };

        // True while the calling thread runs work for the parallel scheduler (a completion or a bulk item)
        bool in_parallel_work() noexcept;

        // Marks the calling thread, while alive, as one that must never block in sync_wait. The IO context's
        // bodies use it: a wait there could deadlock the completions it waits for.
        class forbid_blocking_waits
        {
        public:
            forbid_blocking_waits() noexcept;
            ~forbid_blocking_waits();
            forbid_blocking_waits(const forbid_blocking_waits&) = delete;
            forbid_blocking_waits& operator=(const forbid_blocking_waits&) = delete;
        };

        // True while the calling thread runs parallel work or is inside a forbid_blocking_waits scope
        bool blocking_wait_forbidden() noexcept;

        template<typename Error>
        avis::error to_internal_error(Error&& error) noexcept
        {
            using error_type = std::remove_cvref_t<Error>;
            if constexpr (std::same_as<error_type, avis::error>)
            {
                return std::forward<Error>(error);
            }
            else if constexpr (std::same_as<error_type, std::exception_ptr>)
            {
                return make_error(error);
            }
            else if constexpr (std::same_as<error_type, std::error_code>)
            {
                return avis::error{ .domain = error_domain::generic,
                                    .code = error.value(),
                                    .message = error.message() };
            }
            else
            {
                return make_error(generic_error_kind::unhandled_exception, "unknown error type in a sender pipeline");
            }
        }
    } // namespace detail

    // Sender adaptor that turns every error of the predecessor into avis::error, for example the std::exception_ptr
    // that stdexec::then reports when its function throws:
    //   schedule(stdexec::get_parallel_scheduler()) | then(parse) | into_internal_error()
    inline auto into_internal_error()
    {
        return stdexec::let_error(
            []<typename Error>(Error&& error) noexcept
            { return stdexec::just_error(detail::to_internal_error(std::forward<Error>(error))); });
    }

    // stdexec::sync_wait for code that may run as parallel work or on an IO thread: blocking there could deadlock the
    // work it waits for, so this throws std::logic_error there instead
    template<stdexec::sender Sender>
    auto sync_wait(Sender&& sender)
    {
        if (detail::blocking_wait_forbidden())
        {
            throw std::logic_error{ "avis::threading::sync_wait called from parallel work or an IO thread; compose "
                                    "with let_value or when_all instead" };
        }
        return stdexec::sync_wait(std::forward<Sender>(sender));
    }

    namespace detail
    {
        // result type for one value signature set_value_t(Values...)
        template<typename... Values>
        struct result_for
        {
            using type = result<std::tuple<Values...>>;
        };

        template<>
        struct result_for<>
        {
            using type = result<void>;
        };

        template<typename Value>
        struct result_for<Value>
        {
            using type = result<Value>;
        };

        template<typename... Values>
        using result_for_t = typename result_for<std::decay_t<Values>...>::type;

        // The one result type of a sender's value signatures; result<void> when it has none (it only fails or stops)
        template<typename... Results>
        struct single_result
        {
            static_assert(
                sizeof...(Results) == 0,
                "avis::threading::sync_wait_result: senders with more than one value signature aren't supported");
            using type = result<void>;
        };

        template<typename Result>
        struct single_result<Result>
        {
            using type = Result;
        };

        template<typename Sender>
        using sync_wait_result_t =
            typename stdexec::value_types_of_t<Sender, stdexec::env<>, result_for_t, single_result>::type;
    } // namespace detail

    // Like sync_wait, but turns the three channels into a result (docs/design/07-io-layer.md §5.2):
    //   set_value(T)   -> result<T>   (no values: result<void>; several values: result<std::tuple<...>>)
    //   set_error(E)   -> unexpected(to_internal_error(E))
    //   set_stopped()  -> unexpected(generic_error_kind::cancelled)
    // Throws std::logic_error on a parallel worker or an IO thread, like sync_wait.
    template<stdexec::sender Sender>
    auto sync_wait_result(Sender&& sender) -> detail::sync_wait_result_t<Sender>
    {
        using result_type = detail::sync_wait_result_t<Sender>;

        auto as_result =
            std::forward<Sender>(sender) |
            stdexec::then(
                []<typename... Values>(Values&&... values) -> result_type
                {
                    if constexpr (sizeof...(Values) == 0)
                    {
                        return result_type{};
                    }
                    else if constexpr (sizeof...(Values) == 1)
                    {
                        return result_type{ std::in_place, std::forward<Values>(values)... };
                    }
                    else
                    {
                        return result_type{ std::in_place,
                                            typename result_type::value_type{ std::forward<Values>(values)... } };
                    }
                }) |
            // Stopped becomes a value too: sync_wait only accepts senders that can complete with a value, and
            // just_stopped() can't
            stdexec::upon_stopped(
                []() -> result_type
                { return std::unexpected(make_error(generic_error_kind::cancelled, "operation stopped")); }) |
            into_internal_error() |
            stdexec::upon_error(
                [](avis::error error) noexcept -> result_type { return std::unexpected(std::move(error)); });

        auto outcome = avis::threading::sync_wait(std::move(as_result));
        if (!outcome)
        {
            // Not reached: every channel above ends in a value
            return std::unexpected(make_error(generic_error_kind::cancelled, "operation stopped"));
        }
        return std::move(std::get<0>(*outcome));
    }
} // namespace avis::threading

#endif
