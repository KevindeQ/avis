#ifndef AVIS_IO_IO_CONTEXT_H
#define AVIS_IO_IO_CONTEXT_H

#include "avis/io/asio_scheduler.h"
#include "avis/threading/thread_handle.h"

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include <cstddef>
#include <stop_token>
#include <vector>

namespace avis::threading
{
    class thread_pool;
} // namespace avis::threading

namespace avis::io
{
    // An asio::io_context whose run() loops are long-running bodies on a thread pool. The default instance is owned by
    // avis::runtime; tests may construct their own.
    class io_context
    {
    public:
        // Starts `threads` bodies named "io-<n>" on the pool. Throws avis::runtime_error if threads is 0 or the pool
        // can't start them (a startup misconfiguration).
        io_context(threading::thread_pool& pool, std::size_t threads);
        ~io_context(); // request_stop() + join the bodies
        io_context(const io_context&) = delete;
        io_context& operator=(const io_context&) = delete;
        io_context(io_context&&) = delete; // the bodies point at this object
        io_context& operator=(io_context&&) = delete;

        scheduler get_scheduler() noexcept;
        asio::io_context& native() noexcept;

        // Releases the work guard and stops the context. Operations still in flight are abandoned: full
        // cancellation comes with the IO layer.
        void request_stop() noexcept;

        // The io_context whose body runs on the calling thread, nullptr on other threads
        static const io_context* current() noexcept;

    private:
        void run(std::stop_token token); // the body of every IO thread

        asio::io_context context_;
        asio::executor_work_guard<asio::io_context::executor_type> guard_;
        std::vector<threading::thread_handle> threads_;
    };
} // namespace avis::io

#endif
