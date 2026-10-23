#ifndef AVIS_THREADING_PARALLEL_HELPERS_H
#define AVIS_THREADING_PARALLEL_HELPERS_H

#include "avis/error.h"

#include <stdexec/execution.hpp>

#include <concepts>
#include <exception>
#include <stdexcept>
#include <system_error>
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
                return make_error(generic_errc::unhandled_exception, "unknown error type in a sender pipeline");
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
} // namespace avis::threading

#endif
