#include "avis/runtime.h"

#include "avis/io/io_context.h"
#include "avis/log.h"
#include "avis/threading/parallel_backend.h"
#include "avis/threading/thread_pool.h"

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <exception>
#include <format>
#include <stdexcept>
#include <string_view>
#include <utility>

// AVIS_LIB is the only definition of stdexec's backend query. stdexec's own default would be a second one.
#if defined(STDEXEC_PARALLEL_SCHEDULER_HEADER_ONLY) || defined(STDEXEC_SYSTEM_CONTEXT_HEADER_ONLY)
#    error "AVIS_LIB defines the parallel scheduler backend"
#endif

namespace avis
{
    namespace
    {
        // The live runtime's parallel backend, returned by the backend query below. Set at the end of construction
        // step 5 and cleared at the start of shutdown, so get_parallel_scheduler() throws outside that window.
        std::atomic<std::shared_ptr<threading::parallel_backend>> published_backend;

        std::atomic<bool> accessors_throw = false;
    } // namespace

    std::atomic<runtime*> runtime::instance_ = nullptr;
    std::atomic<bool> runtime::constructing_ = false;

    struct runtime::state
    {
        runtime_configuration configuration;
        bool logging = false;
        std::unique_ptr<threading::thread_pool> pool;
        std::unique_ptr<io::io_context> io;
        std::unique_ptr<exec::static_thread_pool> parallel_pool;
        std::shared_ptr<threading::parallel_backend> backend; // set only once it's published
    };

    runtime::runtime(const runtime_configuration& configuration) : state_{ std::make_unique<state>() }
    {
        if (constructing_.exchange(true, std::memory_order_acq_rel))
        {
            throw runtime_error{ make_error(generic_errc::already_exists, "runtime: another runtime is alive") };
        }

        try
        {
            auto resolved = runtime_configuration::resolve(configuration);
            if (!resolved)
            {
                throw runtime_error{ std::move(resolved.error()) };
            }
            state_->configuration = std::move(*resolved);
            const runtime_configuration& active = state_->configuration;

            if (auto logging = log::detail::initialise(active.logging); !logging)
            {
                throw runtime_error{ std::move(logging.error()) };
            }
            state_->logging = true;

            state_->pool = std::make_unique<threading::thread_pool>(active.thread_pool);
            state_->io = std::make_unique<io::io_context>(*state_->pool, active.io.threads);

            state_->parallel_pool =
                std::make_unique<exec::static_thread_pool>(static_cast<std::uint32_t>(active.parallel.workers));
            auto backend = std::make_shared<threading::parallel_backend>(*state_->parallel_pool);
            published_backend.store(backend);
            state_->backend = std::move(backend);
        }
        catch (const runtime_error&)
        {
            shut_down();
            constructing_.store(false, std::memory_order_release);
            throw;
        }
        catch (...)
        {
            auto failure = make_error(std::current_exception());
            shut_down();
            constructing_.store(false, std::memory_order_release);
            throw runtime_error{ std::move(failure) };
        }

        instance_.store(this, std::memory_order_release);
        log::get("core")->info(
            "runtime: started (thread pool capacity {}, {} IO threads, {} parallel workers)",
            state_->configuration.thread_pool.capacity,
            state_->configuration.io.threads,
            state_->configuration.parallel.workers);
    }

    runtime::~runtime()
    {
        shut_down();
        constructing_.store(false, std::memory_order_release);
    }

    void runtime::shut_down() noexcept
    {
        state& services = *state_;

        // 1. Parallel pool: new get_parallel_scheduler() calls throw, earlier copies
        //    complete with set_stopped, then the workers stop. Work still queued may never run (D-63).
        if (services.backend)
        {
            published_backend.store(nullptr);
            services.backend->detach();
            services.backend.reset();
        }
        shutdown_step("parallel pool", [&] { services.parallel_pool.reset(); });

        // 2. IO: stop the context and join its bodies
        shutdown_step("io context", [&] { services.io.reset(); });

        // 3. Unpublish, only now, so the last parallel work and IO completions could still use the accessors
        runtime* expected = this;
        instance_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);

        // 4. Thread pool: the backstop for application threads that weren't stopped
        shutdown_step("thread pool", [&] { services.pool.reset(); });

        // 5. Logging
        if (services.logging)
        {
            log::detail::shutdown();
            services.logging = false;
        }
    }

    threading::thread_pool& runtime::thread_pool()
    {
        runtime* const live = live_instance("thread_pool");
        return *live->state_->pool;
    }

    io::io_context& runtime::io_context()
    {
        runtime* const live = live_instance("io_context");
        if (!live->state_->io)
        {
            throw std::logic_error{ "avis::runtime::io_context() called while the IO context is shutting down" };
        }
        return *live->state_->io;
    }

    bool runtime::alive() noexcept
    {
        return instance_.load(std::memory_order_acquire) != nullptr;
    }

    const runtime_configuration& runtime::configuration()
    {
        return live_instance("configuration")->state_->configuration;
    }

    runtime* runtime::live_instance(const std::string_view accessor)
    {
        runtime* const live = instance_.load(std::memory_order_acquire);
        if (live == nullptr)
        {
#ifndef NDEBUG
            if (!accessors_throw.load(std::memory_order_relaxed))
            {
                assert(false && "an avis::runtime static accessor was called with no runtime alive");
            }
#endif
            throw std::logic_error{ std::format("avis::runtime::{}() called with no runtime alive", accessor) };
        }
        return live;
    }

    namespace detail
    {
        void make_runtime_accessors_throw(const bool enabled) noexcept
        {
            accessors_throw.store(enabled, std::memory_order_relaxed);
        }
    } // namespace detail
} // namespace avis

// Avis's definition of the backend query that stdexec::get_parallel_scheduler() calls: the live runtime's backend, or
// nullptr (get_parallel_scheduler() then throws)
namespace STDEXEC::parallel_scheduler_replacement
{
    auto query_parallel_scheduler_backend() -> std::shared_ptr<parallel_scheduler_backend>
    {
        return avis::published_backend.load();
    }
} // namespace STDEXEC::parallel_scheduler_replacement
