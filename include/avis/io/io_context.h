#ifndef AVIS_IO_IO_CONTEXT_H
#define AVIS_IO_IO_CONTEXT_H

#include "avis/io/asio_scheduler.h"
#include "avis/io/io_configuration.h"
#include "avis/threading/thread_handle.h"

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stop_token>
#include <vector>

namespace avis::threading
{
    class thread_pool;
} // namespace avis::threading

namespace avis::io
{
    namespace detail
    {
        class operation_registry;
    } // namespace detail

    // An asio::io_context whose run() loops are long-running bodies on a thread pool. The default instance is owned by
    // avis::runtime; tests may construct their own.
    class io_context
    {
    public:
        // Starts configuration.threads bodies named "io-<n>" on the pool. Throws avis::runtime_error if threads is 0 or
        // the pool can't start them (a startup misconfiguration).
        io_context(threading::thread_pool& pool, const io_configuration& configuration);
        ~io_context(); // request_stop() + join the bodies
        io_context(const io_context&) = delete;
        io_context& operator=(const io_context&) = delete;
        io_context(io_context&&) = delete; // the bodies point at this object
        io_context& operator=(io_context&&) = delete;

        scheduler get_scheduler() noexcept;
        asio::io_context& native() noexcept;

        // Cancels every registered operation (they complete with set_stopped), releases the work guard, waits up to
        // the configured shutdown timeout for the cancelled operations to complete, then stops the context
        // (docs/design/07-io-layer.md §3.3). New operations complete with set_stopped from then on. Called from one of
        // this context's IO threads it doesn't wait. Idempotent.
        void request_stop() noexcept;

        // True once request_stop() began
        bool stopping() const noexcept;

        // The io_context whose body runs on the calling thread, nullptr on other threads
        static const io_context* current() noexcept;

    private:
        void run(std::stop_token token); // the body of every IO thread

        asio::io_context context_;
        detail::operation_registry* operations_; // an asio service of context_, which owns it
        asio::executor_work_guard<asio::io_context::executor_type> guard_;
        std::chrono::milliseconds shutdown_timeout_;
        std::atomic<bool> stopping_{ false };
        std::vector<threading::thread_handle> threads_;
    };
} // namespace avis::io

#endif
