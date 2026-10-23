#include "avis/io/io_context.h"

#include "avis/error.h"
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

    io_context::io_context(threading::thread_pool& pool, const std::size_t threads) :
        context_{ static_cast<int>(threads) },
        guard_{ asio::make_work_guard(context_) }
    {
        if (threads == 0)
        {
            throw runtime_error{ make_error(
                generic_errc::invalid_configuration, "io_context: needs at least 1 thread") };
        }

        threads_.reserve(threads);
        for (std::size_t index = 0; index < threads; ++index)
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
        guard_.reset();
        context_.stop();
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
