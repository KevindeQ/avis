#ifndef AVIS_THREADING_THREAD_POOL_H
#define AVIS_THREADING_THREAD_POOL_H

#include "avis/error.h"
#include "avis/threading/thread_handle.h"
#include "avis/threading/thread_options.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace avis::threading
{
    namespace detail
    {
        class pool_thread;
    } // namespace detail

    struct thread_pool_configuration
    {
        std::size_t capacity = 16; // maximum number of concurrently running long-running threads
        std::chrono::milliseconds join_warning{ 5000 }; // log threads that haven't exited after this long
    };

    // A bounded set of named, long-lived OS threads, one body each. Knows nothing about tasks, queues or
    // senders. The default instance is owned by avis::runtime; tests may construct their own.
    class thread_pool
    {
    public:
        explicit thread_pool(const thread_pool_configuration& configuration);
        ~thread_pool(); // request_stop() + join()
        thread_pool(const thread_pool&) = delete;
        thread_pool& operator=(const thread_pool&) = delete;
        thread_pool(thread_pool&&) = delete; // running threads point at their pool
        thread_pool& operator=(thread_pool&&) = delete;

        // Fails with generic_error_kind::pool_exhausted when capacity() bodies are running, with
        // generic_error_kind::pool_stopping after request_stop(), and with generic_error_kind::logic_error for an empty
        // body
        result<thread_handle> start(std::string_view name, thread_body body, thread_options options = {});

        void request_stop() noexcept; // signals every running body
        void join(); // waits until every thread has exited

        std::size_t capacity() const noexcept;
        std::size_t running() const noexcept;

        // Dense index of the calling pool thread, nullopt on threads not started by a thread_pool
        static std::optional<std::uint32_t> current_index() noexcept;

    private:
        // Called on a pool thread when its body has returned, through the callback start() gives it
        void release_index(std::uint32_t index) noexcept;

        // Requires mutex_. Joins and drops threads that have exited, whether or not their handle joined them.
        void prune();

        // Logs, once on `core`, every thread in `threads` that has not exited join_warning after the stop request.
        // Takes the snapshot join() waits on, so it needs no lock.
        void log_running_threads(const std::vector<std::shared_ptr<detail::pool_thread>>& threads) const;

        thread_pool_configuration configuration_;

        mutable std::mutex mutex_;
        std::vector<std::shared_ptr<detail::pool_thread>> threads_;
        std::vector<bool> used_indices_;
        std::atomic<std::size_t> running_ = 0;
        bool stopping_ = false;
    };
} // namespace avis::threading

#endif
