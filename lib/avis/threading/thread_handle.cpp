#include "avis/threading/thread_handle.h"

#include "pool_thread.h"

#include <utility>

namespace avis::threading
{
    thread_handle::thread_handle(std::shared_ptr<detail::pool_thread> thread) noexcept : thread_{ std::move(thread) } {}

    thread_handle::~thread_handle()
    {
        reset();
    }

    thread_handle& thread_handle::operator=(thread_handle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    thread_handle::operator bool() const noexcept
    {
        return thread_ != nullptr;
    }

    std::string_view thread_handle::name() const noexcept
    {
        return thread_ ? std::string_view{ thread_->name() } : std::string_view{};
    }

    void thread_handle::request_stop() noexcept
    {
        if (thread_)
        {
            thread_->request_stop();
        }
    }

    void thread_handle::join()
    {
        if (thread_)
        {
            thread_->join();
        }
    }

    bool thread_handle::running() const noexcept
    {
        return thread_ && thread_->state() != detail::thread_state::exited;
    }

    std::optional<error> thread_handle::exit_error() const
    {
        return thread_ ? thread_->exit_error() : std::nullopt;
    }

    void thread_handle::reset() noexcept
    {
        if (!thread_)
        {
            return;
        }

        thread_->request_stop();
        try
        {
            thread_->join();
        }
        catch (...)
        {
            // std::thread::join only throws for invalid states, which join() rules out
        }
        thread_.reset();
    }
} // namespace avis::threading
