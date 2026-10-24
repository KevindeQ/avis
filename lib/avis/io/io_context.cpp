#include "avis/io/io_context.h"

#include "detail/operation_registry.h"

#include "avis/error.h"
#include "avis/log.h"
#include "avis/threading/parallel_helpers.h"
#include "avis/threading/thread_pool.h"

#include <format>
#include <stop_token>
#include <utility>

namespace avis::io
{
    namespace
    {
        thread_local const io_context* current_context = nullptr;
    } // namespace

    io_context::io_context(threading::thread_pool& pool, const io_configuration& configuration) :
        context_{ static_cast<int>(configuration.threads) },
        operations_{ &asio::make_service<detail::operation_registry>(context_) },
        guard_{ asio::make_work_guard(context_) },
        shutdown_timeout_{ configuration.shutdown_timeout }
    {
        if (configuration.threads == 0)
        {
            throw runtime_error{ make_error(
                generic_error_kind::invalid_configuration, "io_context: needs at least 1 thread") };
        }

        threads_.reserve(configuration.threads);
        for (std::size_t index = 0; index < configuration.threads; ++index)
        {
            auto handle = pool.start(std::format("io-{}", index), [this](const std::stop_token token) { run(token); });
            if (!handle)
            {
                // The destructor doesn't run for a constructor that throws: stop and join what started
                request_stop();
                threads_.clear();
                throw runtime_error{ std::move(handle.error()) };
            }
            threads_.push_back(std::move(*handle));
        }
    }

    io_context::~io_context()
    {
        request_stop();
        threads_.clear(); // joins the bodies
    }

    scheduler io_context::get_scheduler() noexcept
    {
        return scheduler{ context_ };
    }

    asio::io_context& io_context::native() noexcept
    {
        return context_;
    }

    void io_context::request_stop() noexcept
    {
        if (!stopping_.exchange(true))
        {
            // Cancelled operations complete with set_stopped on the IO threads, which keep running until stop()
            operations_->close_and_cancel_all();
            guard_.reset();

            // Waiting on an IO thread would block the completions it waits for
            if (current() != this && shutdown_timeout_.count() > 0)
            {
                try
                {
                    if (!operations_->wait_empty(shutdown_timeout_))
                    {
                        log::get("io")->warn(
                            "io_context: {} operation(s) still running after the {} ms shutdown timeout; stopping "
                            "without them",
                            operations_->size(),
                            shutdown_timeout_.count());
                    }
                }
                catch (...)
                {
                    // Waiting or logging failed: stop without waiting
                }
            }
        }
        else
        {
            guard_.reset();
        }
        context_.stop();
    }

    bool io_context::stopping() const noexcept
    {
        return stopping_.load();
    }

    const io_context* io_context::current() noexcept
    {
        return current_context;
    }

    void io_context::run(const std::stop_token token)
    {
        // A stop request on the body (its handle, or the pool's backstop) stops the whole context
        const std::stop_callback on_stop{ token, [this]() noexcept { context_.stop(); } };
        const threading::detail::forbid_blocking_waits no_blocking_waits;

        current_context = this;
        try
        {
            context_.run();
        }
        catch (...)
        {
            current_context = nullptr;
            throw; // the pool records it as the body's exit error
        }
        current_context = nullptr;
    }
} // namespace avis::io
