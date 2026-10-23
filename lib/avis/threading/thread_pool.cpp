#include "avis/threading/thread_pool.h"

#include "pool_thread.h"

#include "avis/log.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace avis::threading
{
    thread_pool::thread_pool(const thread_pool_configuration& configuration) :
        configuration_{ configuration },
        used_indices_(configuration.capacity, false)
    {}

    thread_pool::~thread_pool()
    {
        request_stop();
        try
        {
            join();
        }
        catch (...)
        {
            log::get("core")->error("thread pool: join failed during destruction");
        }
    }

    result<thread_handle> thread_pool::start(const std::string_view name, thread_body body, thread_options options)
    {
        std::lock_guard lock{ mutex_ };

        if (stopping_)
        {
            return std::unexpected{ make_error(
                generic_errc::pool_stopping,
                std::format("thread pool: can't start 'avis-{}', the pool is stopping", name)) };
        }

        if (!body)
        {
            return std::unexpected{ make_error(
                generic_errc::logic_error,
                std::format("thread pool: can't start 'avis-{}', the body is empty", name)) };
        }

        prune();

        const auto free_index = std::ranges::find(used_indices_, false);
        if (free_index == used_indices_.end())
        {
            return std::unexpected{ make_error(
                generic_errc::pool_exhausted,
                std::format(
                    "thread pool: can't start 'avis-{}', all {} threads are in use", name, configuration_.capacity)) };
        }

        // Grow threads_ before the thread exists, so tracking it below can't fail: an untracked running thread would
        // call back into a pool that may be gone
        try
        {
            threads_.reserve(threads_.size() + 1);
        }
        catch (...)
        {
            return std::unexpected{ make_error(std::current_exception()) };
        }

        const auto index = static_cast<std::uint32_t>(std::distance(used_indices_.begin(), free_index));
        *free_index = true;
        running_.fetch_add(1, std::memory_order_acq_rel);

        // The new thread calls release_index() when its body returns; it blocks on mutex_ until this call is done
        auto thread = detail::pool_thread::start(
            name, index, std::move(body), std::move(options), [this, index]() noexcept { release_index(index); });
        if (!thread)
        {
            used_indices_[index] = false;
            running_.fetch_sub(1, std::memory_order_acq_rel);
            return std::unexpected{ std::move(thread.error()) };
        }

        threads_.push_back(*thread); // no reallocation, reserved above
        return thread_handle{ std::move(*thread) };
    }

    void thread_pool::request_stop() noexcept
    {
        std::vector<std::shared_ptr<detail::pool_thread>> threads;
        {
            std::lock_guard lock{ mutex_ };
            stopping_ = true;
            threads = threads_;
        }

        // Signal every body before anything waits (signal-all-then-join-all). Outside the lock, because stop
        // callbacks run here and may call back into the pool.
        for (const auto& thread : threads)
        {
            thread->request_stop();
        }
    }

    void thread_pool::join()
    {
        std::vector<std::shared_ptr<detail::pool_thread>> threads;
        {
            std::lock_guard lock{ mutex_ };
            threads = threads_;
        }

        const auto deadline = std::chrono::steady_clock::now() + configuration_.join_warning;
        for (const auto& thread : threads)
        {
            if (thread.get() != detail::pool_thread::current() && !thread->wait_until_exited(deadline))
            {
                log_running_threads(threads);
                break;
            }
        }

        // Waits without a timeout: hangs are diagnosed above, never hidden by detaching
        for (const auto& thread : threads)
        {
            thread->join();
        }

        std::lock_guard lock{ mutex_ };
        prune();
    }

    std::size_t thread_pool::capacity() const noexcept
    {
        return configuration_.capacity;
    }

    std::size_t thread_pool::running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    std::optional<std::uint32_t> thread_pool::current_index() noexcept
    {
        return detail::pool_thread::current_index();
    }

    void thread_pool::release_index(const std::uint32_t index) noexcept
    {
        std::lock_guard lock{ mutex_ };
        used_indices_[index] = false;
        running_.fetch_sub(1, std::memory_order_release);
    }

    void thread_pool::prune()
    {
        // Also joins threads that exited without a handle joining them, e.g. a thread whose on_exit replaced its own
        // handle. Joining under mutex_ is safe: an exited thread only has the end of its wrapper left to run.
        std::erase_if(threads_, [](const std::shared_ptr<detail::pool_thread>& thread) { return thread->try_join(); });
    }

    void thread_pool::log_running_threads(const std::vector<std::shared_ptr<detail::pool_thread>>& threads) const
    {
        std::string running;
        for (const auto& thread : threads)
        {
            const auto state = thread->state();
            if (state != detail::thread_state::exited)
            {
                running += std::format(
                    "{}avis-{} (os id {}, {})",
                    running.empty() ? "" : ", ",
                    thread->name(),
                    thread->os_id(),
                    detail::to_string(state));
            }
        }

        if (!running.empty())
        {
            log::get("core")->warn(
                "thread pool: threads still running {} ms after the stop request: {}",
                configuration_.join_warning.count(),
                running);
        }
    }
} // namespace avis::threading
