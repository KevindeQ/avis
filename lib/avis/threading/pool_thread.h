#ifndef AVIS_THREADING_POOL_THREAD_H
#define AVIS_THREADING_POOL_THREAD_H

#include "thread_state.h"

#include "avis/error.h"
#include "avis/threading/thread_options.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace avis::threading::detail
{
    // One long-running thread of a thread_pool, with everything that belongs to it: the OS thread, its name, stop
    // source, state and exit error, and the wrapper that runs the body. Shared by the pool (for the backstop shutdown)
    // and the body's thread_handle (RAII ownership), so a handle may outlive its pool.
    // Private to the library.
    class pool_thread
    {
    public:
        // Creates the thread and starts running body on it. release_index is called on the thread once the body has
        // returned, before on_exit runs, so the owner can free the index and slot.
        static result<std::shared_ptr<pool_thread>> start(
            std::string_view name,
            std::uint32_t index,
            thread_body body,
            thread_options options,
            std::move_only_function<void() noexcept> release_index);

        pool_thread(std::string_view name, std::uint32_t index);
        pool_thread(const pool_thread&) = delete;
        pool_thread& operator=(const pool_thread&) = delete;

        // Without the "avis-" prefix
        const std::string& name() const noexcept;
        thread_state state() const noexcept;
        std::uint32_t os_id() const noexcept;

        // A copy of the error if the body exited by exception; nullopt while the thread is still running.
        // Safe to call at any time.
        std::optional<error> exit_error() const;

        // Signals the body. Moves starting/running to stop_requested, which the join warning reports.
        void request_stop() noexcept;

        // Waits until the body and on_exit have finished; false if the deadline passed first
        bool wait_until_exited(std::chrono::steady_clock::time_point deadline);

        // Waits for the body to finish, then joins the OS thread. Idempotent and safe from several owners.
        // Called on the thread itself it returns immediately instead of deadlocking; the pool joins the thread later
        // (see try_join). That is expected from on_exit (a restart that replaces the thread's own handle) and logged
        // as a warning from the body.
        void join();

        // Joins the OS thread if the body and on_exit have finished, without waiting for them. True once the thread
        // is joined. Lets the pool reclaim threads that no handle joined.
        bool try_join() noexcept;

        // Dense index of the calling pool thread while its body runs, nullopt elsewhere
        static std::optional<std::uint32_t> current_index() noexcept;

        // The calling pool thread, nullptr on other threads
        static const pool_thread* current() noexcept;

    private:
        // The body of the OS thread: configures it, runs body, calls release_index, calls on_exit and marks the
        // thread exited. Invoked through a shared_ptr held by std::thread, which keeps the object alive meanwhile.
        // body and on_exit are destroyed before the thread is marked exited, so a try_join() after that point
        // only waits for the end of run(), never for user code.
        void run(thread_body body, thread_options options, std::move_only_function<void() noexcept> release_index);

        // Name, priority and affinity of the calling OS thread; called by run() on the new thread
        void configure_os_thread(const thread_options& options);

        // "avis-<name>": the name shown in logs, debuggers and crash dumps
        std::string full_name() const;

        // The calling pool thread and its index while its body runs; nullptr / nullopt on other threads
        static thread_local const pool_thread* current_thread_;
        static thread_local std::optional<std::uint32_t> current_index_;

        std::string name_;
        std::uint32_t index_ = 0;
        std::stop_source stop_source_;
        std::atomic<thread_state> state_ = thread_state::starting;
        std::atomic<std::uint32_t> os_id_ = 0;

        // Written by the thread before exited_ is set; read only after that
        std::optional<error> exit_error_;

        // Signalled when the body and on_exit have finished
        mutable std::mutex exit_mutex_;
        std::condition_variable exited_condition_;
        bool exited_ = false;

        // The OS thread, joined exactly once under join_mutex_
        std::mutex join_mutex_;
        std::thread thread_;
    };
} // namespace avis::threading::detail

#endif
