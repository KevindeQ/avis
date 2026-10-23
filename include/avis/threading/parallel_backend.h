#ifndef AVIS_THREADING_PARALLEL_BACKEND_H
#define AVIS_THREADING_PARALLEL_BACKEND_H

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <span>

namespace avis::threading
{
    // stdexec parallel_scheduler backend that runs work on an exec::static_thread_pool it doesn't own. The runtime
    // creates the one stdexec::get_parallel_scheduler() uses; everything else goes through that.
    class parallel_backend final : public stdexec::parallel_scheduler_replacement::parallel_scheduler_backend
    {
    public:
        using receiver_proxy = stdexec::parallel_scheduler_replacement::receiver_proxy;
        using bulk_item_receiver_proxy = stdexec::parallel_scheduler_replacement::bulk_item_receiver_proxy;

        // The pool must outlive every operation started through this backend, or detach() must be called first
        explicit parallel_backend(exec::static_thread_pool& pool);

        void schedule(receiver_proxy& receiver, std::span<std::byte> scratch) noexcept override;
        void schedule_bulk_chunked(
            std::size_t count, bulk_item_receiver_proxy& receiver, std::span<std::byte> scratch) noexcept override;
        void schedule_bulk_unchunked(
            std::size_t count, bulk_item_receiver_proxy& receiver, std::span<std::byte> scratch) noexcept override;

        // From now on every operation completes with set_stopped() without touching the pool. Call it before destroying
        // the pool, because schedulers copied earlier keep this backend alive.
        void detach() noexcept;

    private:
        exec::static_thread_pool::scheduler scheduler_;
        std::size_t thread_count_ = 0;
        std::atomic<bool> detached_ = false;
    };
} // namespace avis::threading

#endif
