#ifndef AVIS_THREADING_THREAD_HANDLE_H
#define AVIS_THREADING_THREAD_HANDLE_H

#include "avis/error.h"

#include <memory>
#include <optional>
#include <string_view>

namespace avis::threading
{
    namespace detail
    {
        class pool_thread;
    } // namespace detail

    // Move-only owner of one long-running body started on a thread_pool. Destroying the handle requests the
    // body to stop and joins its thread.
    class thread_handle
    {
    public:
        thread_handle() noexcept = default;
        explicit thread_handle(std::shared_ptr<detail::pool_thread> thread) noexcept;
        ~thread_handle();

        thread_handle(thread_handle&& other) noexcept = default;
        thread_handle& operator=(thread_handle&& other) noexcept;
        thread_handle(const thread_handle&) = delete;
        thread_handle& operator=(const thread_handle&) = delete;

        // False for a default-constructed or moved-from handle
        explicit operator bool() const noexcept;

        std::string_view name() const noexcept;

        void request_stop() noexcept; // signals this body only
        void join(); // waits for this body's thread to exit
        bool running() const noexcept;

        // A copy of the error if the body exited by exception; nullopt while it is still running and for an empty
        // handle. Safe to call at any time.
        std::optional<error> exit_error() const;

    private:
        void reset() noexcept;

        std::shared_ptr<detail::pool_thread> thread_;
    };
} // namespace avis::threading

#endif
