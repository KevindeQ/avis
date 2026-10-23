#include "pool_thread.h"

#include "avis/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <utility>
#include <windows.h>

namespace avis::threading::detail
{
    thread_local const pool_thread* pool_thread::current_thread_ = nullptr;
    thread_local std::optional<std::uint32_t> pool_thread::current_index_;

    result<std::shared_ptr<pool_thread>> pool_thread::start(
        const std::string_view name,
        const std::uint32_t index,
        thread_body body,
        thread_options options,
        std::move_only_function<void() noexcept> release_index)
    {
        try
        {
            auto thread = std::make_shared<pool_thread>(name, index);

            // The shared_ptr is the object argument of run(): the OS thread holds its own reference, so the object
            // lives while the body runs
            thread->thread_ =
                std::thread{ &pool_thread::run, thread, std::move(body), std::move(options), std::move(release_index) };
            return thread;
        }
        catch (...)
        {
            return std::unexpected{ make_error(std::current_exception()) };
        }
    }

    pool_thread::pool_thread(const std::string_view name, const std::uint32_t index) : name_{ name }, index_{ index } {}

    const std::string& pool_thread::name() const noexcept
    {
        return name_;
    }

    thread_state pool_thread::state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    std::uint32_t pool_thread::os_id() const noexcept
    {
        return os_id_.load(std::memory_order_relaxed);
    }

    std::optional<error> pool_thread::exit_error() const
    {
        std::lock_guard lock{ exit_mutex_ };
        return exited_ ? exit_error_ : std::nullopt;
    }

    void pool_thread::request_stop() noexcept
    {
        auto expected = thread_state::running;
        if (!state_.compare_exchange_strong(expected, thread_state::stop_requested))
        {
            expected = thread_state::starting;
            state_.compare_exchange_strong(expected, thread_state::stop_requested);
        }
        stop_source_.request_stop();
    }

    bool pool_thread::wait_until_exited(const std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock lock{ exit_mutex_ };
        return exited_condition_.wait_until(lock, deadline, [this] { return exited_; });
    }

    void pool_thread::join()
    {
        if (current_thread_ == this)
        {
            // current_index_ is reset once the body has returned, so it tells the body (a likely bug) from on_exit
            // (a restart replacing this thread's own handle, which is fine)
            if (current_index_)
            {
                log::get("core")->warn(
                    "thread pool: thread '{}' tried to join itself; the join is left to its pool", full_name());
            }
            return;
        }

        {
            std::unique_lock lock{ exit_mutex_ };
            exited_condition_.wait(lock, [this] { return exited_; });
        }

        std::lock_guard lock{ join_mutex_ };
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    bool pool_thread::try_join() noexcept
    {
        if (state() != thread_state::exited || current_thread_ == this)
        {
            return false;
        }

        // Another owner is joining it: leave it to them, a later call sees it joined
        std::unique_lock lock{ join_mutex_, std::try_to_lock };
        if (!lock.owns_lock())
        {
            return false;
        }

        if (thread_.joinable())
        {
            try
            {
                thread_.join();
            }
            catch (...)
            {
                return false;
            }
        }
        return true;
    }

    std::optional<std::uint32_t> pool_thread::current_index() noexcept
    {
        return current_index_;
    }

    const pool_thread* pool_thread::current() noexcept
    {
        return current_thread_;
    }

    // The function run by every pool thread
    // Called through the shared_ptr that std::thread keeps until this returns, so the thread holds its own
    // reference to the object while the body runs.
    void pool_thread::run(
        thread_body body, thread_options options, std::move_only_function<void() noexcept> release_index)
    {
        const std::string thread_name = full_name();

        current_thread_ = this;
        current_index_ = index_;
        os_id_.store(GetCurrentThreadId(), std::memory_order_relaxed);
        log::set_thread_name(thread_name);
        configure_os_thread(options);

        auto expected = thread_state::starting;
        state_.compare_exchange_strong(expected, thread_state::running, std::memory_order_acq_rel);

        try
        {
            body(stop_source_.get_token());
        }
        catch (...)
        {
            exit_error_ = make_error(std::current_exception());
            log::get("core")->error("thread '{}' exited with an exception: {}", thread_name, *exit_error_);
        }

        // Free the index before on_exit, so a restart from on_exit finds a free slot. The owner outlives this call:
        // the pool joins every thread before it is destroyed.
        release_index();
        current_index_.reset();
        body = nullptr; // releases what the body captured

        if (options.on_exit)
        {
            try
            {
                options.on_exit(exit_error_);
            }
            catch (...)
            {
                log::get("core")->error(
                    "thread '{}': on_exit threw: {}", thread_name, make_error(std::current_exception()));
            }
            options.on_exit = nullptr;
        }

        {
            std::lock_guard lock{ exit_mutex_ };
            state_.store(thread_state::exited, std::memory_order_release);
            exited_ = true;
        }
        exited_condition_.notify_all();

        current_thread_ = nullptr;
        log::set_thread_name("");
    }

    // Name, priority and affinity of the calling OS thread. Failures are logged, not fatal.
    void pool_thread::configure_os_thread(const thread_options& options)
    {
        const std::string thread_name = full_name();

        const int length =
            MultiByteToWideChar(CP_UTF8, 0, thread_name.data(), static_cast<int>(thread_name.size()), nullptr, 0);
        std::wstring wide_name(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, thread_name.data(), static_cast<int>(thread_name.size()), wide_name.data(), length);
        if (FAILED(SetThreadDescription(GetCurrentThread(), wide_name.c_str())))
        {
            log::get("core")->warn("thread pool: failed to set the description of thread '{}'", thread_name);
        }

        if (options.priority != thread_priority::normal &&
            !SetThreadPriority(GetCurrentThread(), to_win32_priority(options.priority)))
        {
            log::get("core")->warn("thread pool: failed to set the priority of thread '{}'", thread_name);
        }

        if (options.affinity_mask &&
            SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(*options.affinity_mask)) == 0)
        {
            log::get("core")->warn("thread pool: failed to set the affinity of thread '{}'", thread_name);
        }
    }

    std::string pool_thread::full_name() const
    {
        return "avis-" + name_;
    }
} // namespace avis::threading::detail
